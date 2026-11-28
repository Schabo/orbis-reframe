// The GoPro MAX as docs/formats/gopro-max.md measured it: the sphere already
// dewarped by the camera and stored as an equi-angular cubemap -- two HEVC
// streams of three cube faces each (the second not adjacent to the first),
// the IMU as GPMF in a `gpmd` data track, and a .THM JPEG preview and .LRV
// proxy beside the clip. The reader fills ClipMetadata and the IMU track;
// the cube->sphere assembly is the stitch's job (Projection::EquiAngularCube).

#include "GoproReader.h"

#include "ReaderUtil.h"

#include <orbis-core/gopro/Gpmf.h>
#include <orbis-core/media/LensDemuxer.h>
#include <orbis-core/media/StillPicture.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <system_error>

namespace camera {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr double kG = 9.80665;

// What has been measured, per model and recording mode
// (docs/formats/gopro-max.md, *Modes and sample clips*). A new mode is a new
// row: nothing below reads a size or a model's name itself.
const GoproMode kModes[] = {
    // The MAX's 5.6K: the layout measured against FFmpeg's stitch and
    // stated by the file's PMOD; the axes and the lag measured from the
    // picture's own frame-to-frame turn on two clips (*Gyro axes*).
    {"GoPro Max", 4096, 1344, "5.6K", {1344, 1376, 64}, "-y,x,z", 0.010},
};

// A model or mode not in the table: the MAX's IMU axes and picture lag, no
// layout of its own (the file's, else the size's relations, see read()).
const GoproMode kUnmeasured = {"", 0, 0, "", {}, "-y,x,z", 0.010};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string avError(int rc) {
    char why[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, why, sizeof(why));
    return why;
}

void logUnread(const std::string& path, const char* what, const std::string& err) {
    std::fprintf(stderr, "[camera] %s: can't read the %s: %s\n", path.c_str(), what, err.c_str());
}

// The file's first `n` bytes, ""-short on a shorter file, empty on a read
// error. The maker note sits near the front of every `.360` measured (the
// serial and `GoPro Max` at offset 130), so this is enough to claim a file.
Bytes readHead(const std::string& path, size_t n) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    Bytes b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    b.resize(static_cast<size_t>(std::max(f.gcount(), static_cast<std::streamsize>(0))));
    return b;
}

// Where the maker note starts: `GoPro ` followed by the model's own name.
// The hdlr boxes carry "GoPro AVC" / "GoPro AMB" / "GoPro TCD" strings too,
// and in a fast-start file those could come first; the maker note is the
// only one that names the camera ("GoPro Max", "GoPro Hero13 Black").
size_t findMaker(const Bytes& head) {
    static const char kMark[] = "GoPro ";
    if (head.size() < sizeof(kMark))
        return std::string::npos;
    for (size_t i = 0; i + sizeof(kMark) <= head.size(); ++i) {
        if (!std::equal(std::begin(kMark), std::end(kMark) - 1, head.begin() + i))
            continue;
        const size_t w = i + 6;
        const auto at = [&](const char* s, size_t n) {
            return w + n <= head.size() &&
                   std::equal(s, s + n, head.begin() + w,
                              [](char a, char b) { return std::tolower(a) == std::tolower(b); });
        };
        if (at("Max", 3) || at("Hero", 4))
            return i;
    }
    return std::string::npos;
}

// The serial: the maker note carries it under an `SNb` tag whose next byte
// is a length, the serial itself ending at the NUL before `GoPro `. Falls
// back to the printable run before the NUL when the tag isn't there.
std::string printableBefore(const Bytes& head, size_t pos) {
    for (size_t q = pos > 24 ? pos - 24 : 0; q + 4 <= pos; ++q) {
        if (head[q] == 'S' && head[q + 1] == 'N' && head[q + 2] == 'b') {
            const size_t begin = q + 4; // past the tag and its length byte
            if (begin < pos) {
                size_t end = pos; // the serial ends at its NUL
                while (end > begin && head[end - 1] == 0)
                    --end;
                return std::string(reinterpret_cast<const char*>(head.data() + begin),
                                   end - begin);
            }
        }
    }
    size_t begin = pos;
    while (begin && head[begin - 1] == 0)
        --begin;
    while (begin && std::isprint(head[begin - 1]))
        --begin;
    return std::string(reinterpret_cast<const char*>(head.data() + begin), pos - begin);
}

// The model after the maker note, up to the NUL padding.
std::string modelAfter(const Bytes& head, size_t pos) {
    size_t end = pos + 6; // past "GoPro "
    while (end < head.size() && head[end] != 0)
        ++end;
    return std::string(reinterpret_cast<const char*>(head.data() + pos), end - pos);
}

// An open file and its GPMF sensor track: the data track whose samples are
// GPMF (tag `gpmd`; the first sample opens with `DEVC`). Closed with it.
struct GpmdFile {
    AVFormatContext* fmt = nullptr;
    int gpmd = -1;
    ~GpmdFile() {
        if (fmt)
            avformat_close_input(&fmt);
    }
    bool open(const std::string& path) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
            fmt = nullptr;
            return false;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            avformat_close_input(&fmt);
            return false;
        }
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVCodecParameters* p = fmt->streams[i]->codecpar;
            if (p->codec_type == AVMEDIA_TYPE_DATA &&
                (p->codec_tag == MKTAG('g', 'p', 'm', 'd') || p->codec_tag == MKTAG('d', 'e', 'v', 'c'))) {
                gpmd = static_cast<int>(i);
                break;
            }
        }
        return true;
    }
    std::string formatTag(const char* key) const {
        const AVDictionaryEntry* e = av_dict_get(fmt->metadata, key, nullptr, 0);
        return e && e->value ? e->value : "";
    }
    // Every sample of the sensor track: its presentation time in
    // microseconds and its bytes, in order. The other streams are discarded
    // unread. False, with `err`, when the track can't be read to its end.
    bool samples(std::vector<std::pair<int64_t, Bytes>>& out, std::string& err) {
        out.clear();
        if (gpmd < 0)
            return true;
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            fmt->streams[i]->discard =
                static_cast<int>(i) == gpmd ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            err = "out of memory reading the GoPro telemetry";
            return false;
        }
        const AVRational us{1, AV_TIME_BASE};
        int rc = 0;
        while ((rc = av_read_frame(fmt, pkt)) >= 0) {
            if (pkt->stream_index == gpmd && pkt->size > 0 && pkt->pts != AV_NOPTS_VALUE)
                out.emplace_back(av_rescale_q(pkt->pts, fmt->streams[gpmd]->time_base, us),
                                 Bytes(pkt->data, pkt->data + pkt->size));
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        if (rc < 0 && rc != AVERROR_EOF) {
            err = "the GoPro telemetry can't be read: " + avError(rc);
            return false;
        }
        return true;
    }
    // The camera clock (STMP, microseconds) at the start of the sensor
    // track's first sample: its CameraOrientation's, whose row 0 is video
    // frame 0, else its first stream's. False when there is none.
    bool firstStmp(int64_t& us) {
        if (gpmd < 0)
            return false;
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            fmt->streams[i]->discard =
                static_cast<int>(i) == gpmd ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        AVPacket* pkt = av_packet_alloc();
        if (!pkt)
            return false;
        bool found = false;
        while (!found && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == gpmd && pkt->size > 0) {
                const orbiscore::GpmfSample s = orbiscore::ParseGpmfSensorSample(pkt->data, pkt->size);
                for (const orbiscore::GpmfStream& st : s)
                    if (st.dataTag == "CORI") {
                        us = st.stmpUs;
                        found = true;
                    }
                if (!found && !s.empty()) {
                    us = s.front().stmpUs;
                    found = true;
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        return found;
    }
};

// A chapter of a GoPro take: `GS`, the two-digit chapter, the take's
// four-digit number (GS012266, GS022266, ...).
struct Chapter {
    int chapter = 0;
    std::string take;
};
std::optional<Chapter> chapterOf(const fs::path& p) {
    const std::string s = p.stem().string();
    if (s.size() != 8 || (s[0] != 'G' && s[0] != 'g') || (s[1] != 'S' && s[1] != 's') ||
        !std::all_of(s.begin() + 2, s.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    const int ch = std::stoi(s.substr(2, 2));
    if (ch < 1)
        return std::nullopt;
    return Chapter{ch, s.substr(4)};
}
fs::path chapterPath(const fs::path& like, int chapter, const std::string& take) {
    char name[16];
    std::snprintf(name, sizeof name, "%.2s%02d%s", like.stem().string().c_str(), chapter,
                  take.c_str());
    return like.parent_path() / (name + like.extension().string());
}

// The serial in the maker note of `path`, "" when it has none.
std::string serialOf(const std::string& path) {
    const Bytes head = readHead(path, 65536);
    const size_t mark = findMaker(head);
    return mark == std::string::npos ? std::string() : printableBefore(head, mark);
}

// The chapters after `out`'s own file, as `out.parts` after it: the next
// chapter numbers of the same take beside it, from the same camera, with the
// same streams, each placed by the camera's clock (the sensor track's first
// STMP). Measured on a three-chapter take: each chapter starts exactly where
// the one before ends (482.5600 s on, its video 482.5600 s long). The first
// that doesn't follow on ends the clip there; one that starts later (the
// camera's clock says so) leaves a gap, on the frame grid.
void findChapters(ClipMetadata& out, const orbiscore::InsvStreamInfo& s0) {
    const fs::path self(out.path);
    const auto ch = chapterOf(self);
    if (!ch)
        return;
    int64_t prevStmp = 0;
    {
        GpmdFile f;
        if (!f.open(out.path) || !f.firstStmp(prevStmp))
            return; // no clock to place the next chapter by
    }
    std::error_code ec;
    for (int c = ch->chapter + 1; c <= 99; ++c) {
        const fs::path next = chapterPath(self, c, ch->take);
        if (!fs::exists(next, ec))
            break;
        const std::string np = next.string();
        if (serialOf(np) != out.serial) {
            std::fprintf(stderr, "[camera] %s is named like the next chapter of %s but isn't "
                                 "from the same camera; the clip ends before it\n",
                         np.c_str(), out.path.c_str());
            break;
        }
        orbiscore::LensDemuxer demux;
        std::string err;
        if (!demux.open(np, [](const auto*, const auto*, auto*, std::string&) { return true; },
                        err) ||
            demux.lensCount() != 2 || demux.lens(0).width != s0.width ||
            demux.lens(0).height != s0.height || std::abs(demux.lens(0).fps - s0.fps) > 1e-3) {
            std::fprintf(stderr, "[camera] %s continues %s but its video isn't like it%s%s; the "
                                 "clip ends before it\n",
                         np.c_str(), out.path.c_str(), err.empty() ? "" : ": ", err.c_str());
            break;
        }
        const int64_t durUs = demux.lens(0).durationUs;
        demux.close();
        int64_t stmp = 0;
        {
            GpmdFile f;
            if (!f.open(np) || !f.firstStmp(stmp)) {
                std::fprintf(stderr, "[camera] %s has no camera clock to place it by; the clip "
                                     "ends before it\n",
                             np.c_str());
                break;
            }
        }
        const Part& prev = out.parts.back();
        Part part{np, {{np, 0}, {np, 1}}, prev.startUs + prev.durationUs, durUs, {}};
        const int64_t onUs = stmp - prevStmp;
        const int64_t slackUs = 100000;
        if (onUs < prev.durationUs - slackUs) {
            std::fprintf(stderr, "[camera] %s starts before %s ends (%.3f s on, not %.3f); the "
                                 "clip ends before it\n",
                         np.c_str(), prev.path.c_str(), onUs * 1e-6, prev.durationUs * 1e-6);
            break;
        }
        if (onUs > prev.durationUs + slackUs && s0.fps > 0.0) {
            part.startUs = prev.startUs +
                           std::llround(std::round(onUs * 1e-6 * s0.fps) * 1e6 / s0.fps);
            std::fprintf(stderr, "[camera] %s starts %.3f s after %s, not %.3f: a gap in the "
                                 "clip\n",
                         np.c_str(), onUs * 1e-6, prev.path.c_str(), prev.durationUs * 1e-6);
        }
        out.parts.push_back(std::move(part));
        prevStmp = stmp;
    }
    if (out.parts.size() > 1) {
        const Part& last = out.parts.back();
        out.durationMs = (last.startUs + last.durationUs) / 1000;
        std::fprintf(stderr, "[camera] %s: a take in %zu chapters, %.3f s\n", out.path.c_str(),
                     out.parts.size(), static_cast<double>(last.startUs + last.durationUs) * 1e-6);
    }
}

// The file beside `path` with the same stem and extension `ext` (either
// case), "" when there is none.
std::string sibling(const fs::path& path, const std::string& ext) {
    std::error_code ec;
    std::string up = ext;
    std::transform(up.begin(), up.end(), up.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const std::string& e : {up, lower(ext)}) {
        const fs::path p = path.parent_path() / (path.stem().string() + e);
        if (fs::exists(p, ec))
            return p.string();
    }
    return {};
}

// One motion stream of the track, every sample's rows lined up: the rows
// themselves, the rate measured from dTSMP/dSTMP between samples, and the
// clock pair of each sample (its first row's camera time and the packet's
// video time). STMP is the first row's time: the streams' STMPs inside one
// sample agree to ~1.7 ms, which only holds for a common start.
struct MotionStream {
    double rateHz = 0.0;
    int64_t t0Us = 0; // the first sample's STMP
    std::vector<orbiscore::GpmfStream> chunks;
    std::vector<int64_t> ptsUs; // per chunk: the packet's video time

    // Rows are found by name and data tag; a chunk missing either is skipped.
    static const orbiscore::GpmfStream* find(const std::vector<orbiscore::GpmfStream>& s,
                                             const char* name, const char* tag) {
        for (const orbiscore::GpmfStream& st : s)
            if (st.name == name && st.dataTag == tag)
                return &st;
        return nullptr;
    }

    bool ready() const { return chunks.size() >= 2 && rateHz > 0.0; }

    // The camera time of row `n` of chunk `i`, microseconds: forward from
    // the chunk's STMP at the measured rate (the backward reading puts every
    // join of a clip out of order). A chunk's rows then end ~1 ms before the
    // next chunk's STMP —
    // the per-chunk STMP gaps run slightly longer than the rows they bound —
    // and readImu clamps the join to stay strictly ordered if a chunk's own
    // rate ever dips below the median far enough to close that margin.
    int64_t rowUs(size_t i, size_t n) const {
        return chunks[i].stmpUs +
               static_cast<int64_t>(std::llround(static_cast<double>(n) * 1e6 / rateHz));
    }

    // Measure the rate and clock from the parsed chunks. False when the
    // stream isn't there or its clock doesn't advance.
    bool collect(const std::vector<orbiscore::GpmfSample>& samples,
                 const std::vector<std::pair<int64_t, Bytes>>& packets, const char* name,
                 const char* tag) {
        for (size_t i = 0; i < samples.size() && i < packets.size(); ++i) {
            const orbiscore::GpmfStream* st = find(samples[i], name, tag);
            if (!st || st->payload.empty())
                continue;
            chunks.push_back(*st);
            ptsUs.push_back(packets[i].first);
        }
        if (chunks.size() < 2)
            return false;
        // The rate: the median of the per-chunk dTSMP / dSTMP — 801.589 Hz
        // on a gyro that calls itself 800. The global dTSMP / dSTMP over the
        // whole track is not that: the camera stops mid-sample at the end
        // of a clip, and the last chunk's 128 rows against its full STMP
        // span drag GS012267's global fit down to 790.6 Hz.
        std::vector<double> rates;
        rates.reserve(chunks.size() - 1);
        for (size_t i = 1; i < chunks.size(); ++i) {
            const int64_t dt = chunks[i].stmpUs - chunks[i - 1].stmpUs;
            const uint64_t dn = chunks[i].tsmp - chunks[i - 1].tsmp;
            if (dt > 0 && dn != 0)
                rates.push_back(static_cast<double>(dn) * 1e6 / static_cast<double>(dt));
        }
        if (rates.empty())
            return false;
        std::sort(rates.begin(), rates.end());
        rateHz = rates[rates.size() / 2];
        t0Us = chunks.front().stmpUs;
        return true;
    }
};

// A stream's rows as body-axis doubles: raw / SCAL, then MTRX (measured:
// MTRX rotates sensor axes to camera body axes, applied as MTRX @ v).
std::vector<std::array<double, 3>> bodyRows(const orbiscore::GpmfStream& st) {
    std::vector<std::array<double, 3>> out;
    const std::vector<int16_t> raw = st.rowsI16();
    const size_t rows = raw.size() / 3;
    out.reserve(rows);
    const double scal = st.scal != 0.0 ? st.scal : 1.0;
    for (size_t r = 0; r < rows; ++r) {
        double v[3] = {raw[r * 3] / scal, raw[r * 3 + 1] / scal, raw[r * 3 + 2] / scal};
        double b[3];
        if (st.hasMtrx) {
            for (int k = 0; k < 3; ++k)
                b[k] = st.mtrx[k * 3] * v[0] + st.mtrx[k * 3 + 1] * v[1] +
                       st.mtrx[k * 3 + 2] * v[2];
        } else {
            b[0] = v[0];
            b[1] = v[1];
            b[2] = v[2];
        }
        out.push_back({b[0], b[1], b[2]});
    }
    return out;
}

// The global settings GPMF (moov/udta/GPMF), empty when the file has none
// or can't be read. The MAX writes it at the front, in the `moov` it puts
// before the media.
Bytes readSettingsGpmf(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const uint64_t fileEnd = static_cast<uint64_t>(std::max<std::streamoff>(f.tellg(), 0));
    // The box named `type` among the boxes from `from` to `to`: its payload's
    // start and end.
    const auto find = [&](uint64_t from, uint64_t to, const char* type, uint64_t& start,
                          uint64_t& end) {
        uint64_t at = from;
        while (at + 8 <= to) {
            uint8_t h[16];
            f.clear();
            f.seekg(static_cast<std::streamoff>(at));
            if (!f.read(reinterpret_cast<char*>(h), 8))
                return false;
            uint64_t size = (uint64_t(h[0]) << 24) | (uint64_t(h[1]) << 16) |
                            (uint64_t(h[2]) << 8) | h[3];
            uint64_t header = 8;
            if (size == 1) {
                if (!f.read(reinterpret_cast<char*>(h + 8), 8))
                    return false;
                size = 0;
                for (int k = 8; k < 16; ++k)
                    size = size << 8 | h[k];
                header = 16;
            } else if (size == 0) {
                size = to - at;
            }
            if (size < header || at + size > to)
                return false;
            if (std::equal(type, type + 4, reinterpret_cast<const char*>(h + 4))) {
                start = at + header;
                end = at + size;
                return true;
            }
            at += size;
        }
        return false;
    };
    uint64_t moovA, moovB, udtaA, udtaB, gpmfA, gpmfB;
    if (!find(0, fileEnd, "moov", moovA, moovB) || !find(moovA, moovB, "udta", udtaA, udtaB) ||
        !find(udtaA, udtaB, "GPMF", gpmfA, gpmfB) || gpmfB - gpmfA > (1u << 20))
        return {};
    Bytes b(gpmfB - gpmfA);
    f.clear();
    f.seekg(static_cast<std::streamoff>(gpmfA));
    if (!f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size())))
        return {};
    return b;
}

// A tag's whole-number values (GPMF types b B s S l L j J, big endian);
// empty when the settings don't have it or it holds something else.
std::vector<int64_t> settingsInts(const Bytes& gpmf, const char* tag) {
    std::vector<int64_t> out;
    orbiscore::WalkGpmf(gpmf.data(), gpmf.size(), [&](const orbiscore::GpmfEvent& e) {
        if (e.isStruct || e.tag != std::string_view(tag, 4))
            return true;
        const bool isSigned = std::islower(static_cast<unsigned char>(e.type));
        const size_t width = std::tolower(e.type) == 'b'   ? 1
                             : std::tolower(e.type) == 's' ? 2
                             : std::tolower(e.type) == 'l' ? 4
                             : std::tolower(e.type) == 'j' ? 8
                                                           : 0;
        if (width == 0 || e.payload.size() % width != 0)
            return false;
        for (size_t at = 0; at < e.payload.size(); at += width) {
            uint64_t v = 0;
            for (size_t k = 0; k < width; ++k)
                v = v << 8 | static_cast<uint8_t>(e.payload[at + k]);
            if (isSigned && width < 8 && (v >> (8 * width - 1)) != 0)
                v |= ~uint64_t(0) << (8 * width);
            out.push_back(static_cast<int64_t>(v));
        }
        return false;
    });
    return out;
}

} // namespace

const GoproMode& FindGoproMode(const std::string& model, int width, int height, bool* known) {
    for (const GoproMode& m : kModes)
        if (lower(model) == lower(m.model) && m.width == width && m.height == height) {
            if (known)
                *known = true;
            return m;
        }
    if (known)
        *known = false;
    return kUnmeasured;
}

std::optional<orbiscore::EacLayout> LayoutFromPmod(const std::vector<int64_t>& pmod, int width,
                                                   int height) {
    // PMOD = [32, 1408, 1376] on every 5.6K file: half the overlap, then
    // 1408 (the centre face and the overlap, or a side region and half the
    // overlap: the one mode can't tell), then a side region. The centre is
    // what the side regions leave of the strip, and a face: as tall as wide.
    if (pmod.size() != 3 || pmod[0] <= 0 || pmod[2] <= 0 || pmod[2] > width)
        return std::nullopt;
    orbiscore::EacLayout l;
    l.overlap = static_cast<int>(2 * pmod[0]);
    l.side = static_cast<int>(pmod[2]);
    l.centre = width - 2 * l.side;
    if (!l.fits(width) || l.centre != height)
        return std::nullopt;
    return l;
}

CameraReader::Result GoproReader::read(const std::vector<orbiscore::InsvStreamInfo>& streams,
                                       ClipMetadata& out, std::string& err) const {
    const Bytes head = readHead(out.path, 65536);
    const size_t mark = findMaker(head);
    if (mark == std::string::npos)
        return Result::NotMine;
    // A 360 clip is exactly the two cube streams; a Hero's single-stream
    // MP4 carries the same maker note and isn't ours.
    if (streams.size() != 2)
        return Result::NotMine;

    out.maker = "GoPro";
    out.model = modelAfter(head, mark);
    out.serial = printableBefore(head, mark);
    out.projection = Projection::EquiAngularCube;
    bool known = false;
    const GoproMode& mode = FindGoproMode(out.model, out.lensWidth, out.lensHeight, &known);
    out.untestedModel = !known;
    out.captureMode = mode.name;
    if (!known)
        std::fprintf(stderr, "[camera] %s: no table entry for the %s at %dx%d; its IMU axes and "
                             "timing taken as the MAX's 5.6K, untested\n",
                     out.path.c_str(), out.model.c_str(), out.lensWidth, out.lensHeight);

    // Where the faces are: the file's own numbers (PMOD, in the global
    // settings), else the table's, else the relations the 5.6K layout keeps
    // (centre = the stream's height, side = what's left halved, overlap =
    // twice the side's excess over the centre).
    if (const auto stated =
            LayoutFromPmod(settingsInts(readSettingsGpmf(out.path), "PMOD"), out.lensWidth,
                           out.lensHeight)) {
        out.cubeLayout = *stated;
        if (known && !(mode.layout == *stated))
            std::fprintf(stderr, "[camera] %s: the file's cube layout (PMOD) differs from the "
                                 "table's for the %s; taking the file's\n",
                         out.path.c_str(), mode.name);
    } else if (known) {
        out.cubeLayout = mode.layout;
        std::fprintf(stderr, "[camera] %s: no usable PMOD; the cube layout from the table\n",
                     out.path.c_str());
    } else {
        orbiscore::EacLayout& l = out.cubeLayout;
        l.centre = out.lensHeight;
        l.side = (out.lensWidth - l.centre) / 2;
        l.overlap = 2 * (l.side - l.centre);
        std::fprintf(stderr, "[camera] %s: no usable PMOD; the cube layout taken from the "
                             "stream size as the 5.6K's relations, untested\n",
                     out.path.c_str());
    }

    GpmdFile file;
    if (!file.open(out.path)) {
        err = "the GoPro file can't be opened for its metadata";
        return Result::Failed;
    }
    out.firmware = file.formatTag("firmware");
    // "2025-04-14T15:46:51.000000Z" -> "2025-04-14T15:46:51".
    if (const std::string created = file.formatTag("creation_time"); created.size() >= 19)
        out.recordedLocal = created.substr(0, 19);

    // The two cube streams, in file order: lens A's faces first. No lens
    // calibration is filled: the cube's geometry is the layout above, and the
    // fisheye calibration in the file's settings describes the lenses before
    // the camera made the cube (the .LRV's picture).
    out.lenses.resize(2);
    out.lenses[0].source = {out.path, 0};
    out.lenses[1].source = {out.path, 1};
    out.calibrated = false;

    // The proxy beside it: the .LRV holds the two raw fisheye circles, not
    // cube faces, so it is listed for the Media panel but not wired as
    // Part::proxyLenses (whose picture must show the same sphere the same
    // way).
    const fs::path self(out.path);
    out.proxyPath = sibling(self, ".LRV");

    Part part{out.path, {}, 0, streams[0].durationUs, {}};
    for (const Lens& l : out.lenses)
        part.lenses.push_back(l.source);
    out.parts = {std::move(part)};
    // A take the camera split into chapters: the chapters after this one,
    // placed by the camera's clock.
    findChapters(out, streams[0]);

    // The motion: the gyro's rate, measured from the first two sensor
    // samples rather than assumed.
    std::vector<std::pair<int64_t, Bytes>> packets;
    std::string readErr;
    if (!file.samples(packets, readErr)) {
        // The clip itself is fine; only the motion is unknown.
        logUnread(out.path, "motion", readErr);
    } else if (packets.size() >= 2) {
        std::vector<orbiscore::GpmfSample> samples;
        for (const auto& [pts, bytes] : packets)
            samples.push_back(orbiscore::ParseGpmfSensorSample(bytes.data(), bytes.size()));
        MotionStream gyro;
        if (gyro.collect(samples, packets, "Gyroscope", "GYRO")) {
            out.hasMotion = true;
            out.imuRateHz = gyro.rateHz;
        }
    }
    return Result::Read;
}

// One file's motion: its gyro rows with the nearest accelerometer row, in
// camera body axes, on the file's own clock (frameTimes from its packets),
// mapped and timed by `mode`, the clip's row (its IMU axes and lag).
static std::optional<orbiscore::ImuTrack> imuOfFile(const std::string& path,
                                                    const GoproMode& mode) {
    GpmdFile file;
    std::string err;
    if (!file.open(path)) {
        logUnread(path, "motion", "the file can't be opened again to read its telemetry");
        return std::nullopt;
    }
    std::vector<std::pair<int64_t, Bytes>> packets;
    if (!file.samples(packets, err) || packets.size() < 2) {
        logUnread(path, "motion", err.empty() ? "its sensor track has fewer than two samples"
                                                   : err);
        return std::nullopt;
    }
    std::vector<orbiscore::GpmfSample> samples;
    for (const auto& [pts, bytes] : packets)
        samples.push_back(orbiscore::ParseGpmfSensorSample(bytes.data(), bytes.size()));

    MotionStream gyro, accel;
    if (!gyro.collect(samples, packets, "Gyroscope", "GYRO")) {
        logUnread(path, "motion", "its sensor track has no usable gyroscope");
        return std::nullopt;
    }
    accel.collect(samples, packets, "Accelerometer", "ACCL");

    // The rows, in body axes: gyro rad/s (raw / SCAL, measured against the
    // camera's own CORI attitude to a 0.995 median ratio), accel in g.
    struct Row {
        int64_t us;
        double gyro[3];
        double accel[3];
    };
    std::vector<Row> rows;
    {
        size_t total = 0;
        for (const orbiscore::GpmfStream& c : gyro.chunks)
            total += c.count;
        rows.reserve(total);
    }
    // The accelerometer rides along with each gyro row: its nearest sample
    // (4 gyro rows per accel row at the measured 800/200 Hz; the
    // stabilizer's own low-pass makes the step harmless).
    struct AccelRow {
        int64_t us;
        std::array<double, 3> body; // g
    };
    std::vector<AccelRow> aRows;
    size_t ai = 0; // the nearest accel row so far; times are ascending
    int64_t aPrevUs = INT64_MIN;
    const auto nearestAccel = [&](int64_t us, double out[3]) {
        out[0] = out[1] = out[2] = 0.0;
        if (aRows.empty())
            return;
        while (ai + 1 < aRows.size() && std::abs(aRows[ai + 1].us - us) <= std::abs(aRows[ai].us - us))
            ++ai;
        out[0] = aRows[ai].body[0] / kG;
        out[1] = aRows[ai].body[1] / kG;
        out[2] = aRows[ai].body[2] / kG;
    };

    // A chunk whose own rate sits just above the median can close the ~1 ms
    // margin its rows leave before the next chunk's STMP and land a row on
    // or past the join. The track's clock is the camera's, two rows of one
    // stream never share a microsecond, and the stabilizer integrates
    // differences: keep the join strictly increasing.
    const auto advance = [](int64_t& prev, int64_t us) {
        if (us <= prev)
            us = prev + 1;
        prev = us;
        return us;
    };
    int64_t prevUs = INT64_MIN;

    for (size_t i = 0; i < accel.chunks.size(); ++i) {
        const std::vector<std::array<double, 3>> b = bodyRows(accel.chunks[i]);
        for (size_t n = 0; n < b.size(); ++n)
            aRows.push_back({advance(aPrevUs, accel.rowUs(i, n)), b[n]});
    }

    for (size_t i = 0; i < gyro.chunks.size(); ++i) {
        const std::vector<std::array<double, 3>> g = bodyRows(gyro.chunks[i]);
        for (size_t n = 0; n < g.size(); ++n) {
            Row r;
            r.us = advance(prevUs, gyro.rowUs(i, n));
            r.gyro[0] = g[n][0];
            r.gyro[1] = g[n][1];
            r.gyro[2] = g[n][2];
            nearestAccel(r.us, r.accel);
            rows.push_back(r);
        }
    }
    if (rows.size() < 2) {
        logUnread(path, "motion", "its gyroscope carries no rows");
        return std::nullopt;
    }

    orbiscore::ImuTrack track;
    track.samples.reserve(rows.size());
    for (const Row& r : rows) {
        orbiscore::ImuSample s;
        s.timestamp = static_cast<uint64_t>(r.us);
        for (int k = 0; k < 3; ++k) {
            s.accel[k] = r.accel[k];
            s.gyro[k] = r.gyro[k];
        }
        track.samples.push_back(s);
    }
    // The samples are in camera body axes (MTRX applied above). The map to
    // the stitch's world (+X right, +Y up, -Z forward) isn't in the file: the
    // table's, measured from the picture's own turn.
    track.axisMap = mode.imuAxes;

    // One frameTimes pair per sensor sample: video time is the packet's PTS
    // and the track time that sample's gyro STMP, so each pair is exact by
    // construction and the model interpolates between them (measured: the
    // fit's residuals stay under 0.7 ms, the slope within 5 ppm of 1).
    // The picture's motion lines up with the gyro later than the packet
    // pairs say, by the table's lag -- most likely the sensor's middle row
    // read that long after the frame's time.
    const int64_t t0 = gyro.t0Us;
    for (size_t i = 0; i < gyro.chunks.size(); ++i) {
        track.frameTimes.push_back(
            {static_cast<double>(gyro.ptsUs[i]) / 1e6,
             static_cast<double>(gyro.chunks[i].stmpUs - t0) / 1e6 + mode.pictureLagSec});
    }
    track.hasOffset = true;
    track.offsetSec = track.frameTimes.empty() ? 0.0 : track.frameTimes.front().trackSec;
    track.captureScale = 1.0; // measured: the clocks agree to under 5 ppm
    return track;
}

std::optional<orbiscore::ImuTrack> GoproReader::readImu(const ClipMetadata& clip) const {
    const GoproMode& mode = FindGoproMode(clip.model, clip.lensWidth, clip.lensHeight);
    if (clip.parts.size() <= 1)
        return imuOfFile(clip.path, mode);
    // A take in chapters: each chapter's track by its own file, then one.
    // The chapters run on one camera clock (measured: each starts exactly
    // where the one before ends), and each keeps its own frame times.
    std::vector<orbiscore::ImuTrackPart> tracks;
    for (size_t k = 0; k < clip.parts.size(); ++k) {
        const Part& part = clip.parts[k];
        auto track = imuOfFile(part.path, mode);
        if (!track) {
            std::fprintf(stderr, "[camera] no motion data in %s, chapter %zu of %s\n",
                         part.path.c_str(), k + 1, clip.path.c_str());
            continue;
        }
        tracks.push_back({std::move(*track), static_cast<double>(part.startUs) * 1e-6,
                          static_cast<double>(part.durationUs) * 1e-6});
    }
    if (tracks.empty())
        return std::nullopt;
    return orbiscore::JoinImuTracks(std::move(tracks));
}

std::optional<orbiscore::GpsTrack> GoproReader::readGps(const ClipMetadata& clip) const {
    // Not read yet. The MAX records GPS (GPS5) with GPS switched on; the
    // clips this reader was measured on were filmed with it off and carry
    // none, so there is nothing yet to check a reader against
    // (docs/formats/gopro-max.md, *Files and streams*).
    (void)clip;
    return std::nullopt;
}

std::optional<Thumbnail> GoproReader::readThumbnail(const std::string& path) const {
    const fs::path p(path);
    if (lower(p.extension().string()) != ".360")
        return std::nullopt;
    const std::string thm = sibling(p, ".THM");
    if (thm.empty())
        return std::nullopt;
    std::ifstream f(thm, std::ios::binary);
    if (!f)
        return std::nullopt;
    const Bytes pic((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    orbiscore::Image rgb;
    std::string err;
    if (!orbiscore::DecodeStillPicture(pic, "mjpeg", 1024, rgb, err)) {
        std::fprintf(stderr, "[camera] the preview of %s doesn't decode: %s\n", path.c_str(),
                     err.c_str());
        return std::nullopt;
    }
    // The .THM is the whole sphere, 2:1 equirect (704x352 on the MAX).
    return ThumbnailFromRgb(rgb);
}

std::optional<std::string> GoproReader::clipFileFor(const std::string& path) const {
    const fs::path p(path);
    const std::string ext = lower(p.extension().string());
    // The proxy and the cube file; the .THM preview belongs to the clip too,
    // but a clip claims only its proxy and its lens files, so ClipFileOf
    // would turn it away; the media pool reads it through readThumbnail.
    if (ext != ".lrv" && ext != ".360")
        return std::nullopt;
    if (p.stem().string().compare(0, 2, "GS") != 0)
        return std::nullopt;
    // A proxy names its own chapter's file: a clip claims only its own
    // file's proxy (ClipMetadata::proxyPath).
    if (ext == ".lrv") {
        const std::string own = sibling(p, ".360");
        return own.empty() ? (p.parent_path() / (p.stem().string() + ".360")).string() : own;
    }
    const fs::path clip = p;
    // A later chapter belongs to its take's first, when every chapter
    // between them is there (the first file opens the take, findChapters).
    if (const auto ch = chapterOf(clip); ch && ch->chapter > 1) {
        std::error_code ec;
        fs::path first = clip;
        for (int c = ch->chapter - 1; c >= 1; --c) {
            const fs::path before = chapterPath(clip, c, ch->take);
            if (!fs::exists(before, ec))
                break;
            first = before;
        }
        if (first != clip && chapterOf(first)->chapter == 1)
            return first.string();
    }
    return std::nullopt;
}

} // namespace camera
