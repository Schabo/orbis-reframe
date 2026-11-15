#include "DjiReader.h"

#include "ReaderUtil.h"

#include <DjiMeta.h>
#include <FisheyeProjection.h> // insv::Mat3, rotX / rotY / rotZ
#include <StillPicture.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <system_error>

namespace camera {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr double kDeg = std::numbers::pi / 180.0;

// What differs between DJI's 360 products: the metadata schema, which track
// holds the master lens, how the camera holds its lenses, and how far off
// its axis each lens's picture is usable. Rows only for what our own clips
// show (docs/formats/dji-360.md, *Modes and sample clips*).
struct DjiModel {
    const char* productName; // as clip_meta_header names it
    const char* name;        // as we show it
    const insv::DjiSchema& (*schema)();
    int masterTrack; // the lens stream that holds the master lens
    // ClipMetadata::mountYawDeg / mountPitchDeg / mountRollDeg.
    double mountDeg[3];
    // The usable field of view, degrees (insv::StitchParams::fovDeg).
    double fovDeg;
};

// The Avata 360 flies its camera with the lens axes vertical, the master
// lens up, and the drone's nose a quarter turn from the master picture's
// top: the mount turns the master's axis to the sphere's up and the nose to
// its forward. The nose measured on our clip from the sun against the
// drone's own attitude (docs/formats/dji-360.md §3); the picture reaches
// ~100 deg off each lens's axis before the frame's edge and the housing.
const DjiModel kModels[] = {
    {"DJI Avata360", "DJI Avata 360", &insv::DjiSchemaEagle4, 0, {90.0, -90.0, 90.0}, 200.0},
};

// For a product not in the table: the schema tried in turn, the master on
// the first track, the lens axes as the Avata 360's.
const DjiModel kUnknown = {"", "", &insv::DjiSchemaEagle4, 0, {90.0, -90.0, 90.0}, 200.0};

const DjiModel* findModel(const std::string& productName) {
    for (const DjiModel& m : kModels)
        if (productName == m.productName)
            return &m;
    return nullptr;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// An open MP4 and its first djmd track; closed with it.
struct Mp4 {
    AVFormatContext* fmt = nullptr;
    int djmd = -1;
    ~Mp4() {
        if (fmt)
            avformat_close_input(&fmt);
    }
    bool open(const std::string& path) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
            fmt = nullptr;
            return false;
        }
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVCodecParameters* p = fmt->streams[i]->codecpar;
            if (p->codec_type == AVMEDIA_TYPE_DATA &&
                p->codec_tag == MKTAG('d', 'j', 'm', 'd')) {
                djmd = static_cast<int>(i);
                break;
            }
        }
        return true;
    }
    std::string tag(const char* key) const {
        const AVDictionaryEntry* e = av_dict_get(fmt->metadata, key, nullptr, 0);
        return e && e->value ? e->value : "";
    }
    // Up to `max` samples of the djmd track, in order (0 = all). The other
    // streams are discarded unread.
    std::vector<Bytes> samples(size_t max) {
        std::vector<Bytes> out;
        if (djmd < 0)
            return out;
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            fmt->streams[i]->discard = static_cast<int>(i) == djmd ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        AVPacket* pkt = av_packet_alloc();
        while ((max == 0 || out.size() < max) && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == djmd && pkt->size > 0)
                out.emplace_back(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        return out;
    }
};

// The file beside `path` with the same stem and extension `ext` (either
// case), "" when there is none.
std::string sibling(const fs::path& path, const std::string& stem, const std::string& ext) {
    std::error_code ec;
    std::string upper = ext;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const std::string& e : {upper, lower(ext)}) {
        const fs::path p = path.parent_path() / (stem + e);
        if (fs::exists(p, ec))
            return p.string();
    }
    return {};
}

// "YYYY-MM-DDThh:mm:ss" from a camera name, DJI_YYYYMMDDhhmmss_NNNN_…;
// "" when it isn't named so.
std::string nameTime(const std::string& name) {
    if (name.size() < 18 || name.compare(0, 4, "DJI_") != 0)
        return {};
    return LocalTimeFromDigits(name.substr(4, 14));
}

// A lens's pose from DJI's yaw / pitch / roll (camera from rig,
// Ry(yaw) Rz(roll) Rx(pitch - 90) in image axes, x right and y down;
// docs/formats/dji-360.md §3) in the stitch's terms (insv::LensPose): its
// lens frame is the image's turned half a turn about the optical axis, so
// lens -> rig is that turn on both sides of the transposed rotation. The
// whole pose goes into the mount; the calibration's own angles stay 0.
insv::LensPose lensPose(const insv::DjiDewarp& d) {
    const insv::Mat3 camFromRig =
        insv::mul(insv::mul(insv::rotY(d.yaw * kDeg), insv::rotZ(d.roll * kDeg)),
                  insv::rotX((d.pitch - 90.0) * kDeg));
    const insv::Mat3 flip = insv::rotZ(std::numbers::pi);
    const insv::Mat3 m = insv::mul(insv::mul(flip, insv::transpose(camFromRig)), flip);
    insv::LensPose p;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            p.mount[r * 3 + c] = m.m[r][c];
    p.rollZeroDeg = 0.0;
    return p;
}

// A lens as the clip description takes it: the theta-polynomial in the
// calibration's fields, its frame as lens `index`'s half of a side-by-side
// frame of two (insv::LensCalibExt's convention), the fifth term beside.
Lens lens(const insv::DjiDewarp& d, int index, const LensSource& source) {
    Lens l;
    l.source = source;
    l.model = insv::LensModel::ThetaPoly;
    insv::LensCalibExt& e = l.calibration;
    e.fx = d.fx;
    e.fy = d.fy;
    e.cx = d.cx + index * d.width;
    e.cy = d.cy;
    for (int k = 0; k < 4; ++k)
        e.k[k] = d.k[k];
    e.p[0] = d.p[0];
    e.p[1] = d.p[1];
    e.w = 2.0 * d.width;
    e.h = d.height;
    e.focalRefW = d.width;
    l.k5 = d.k[4];
    l.pose = lensPose(d);
    return l;
}

// The clip metadata of the first sample, parsed by the product's schema,
// or -- for a product not in the table -- by the first schema that yields a
// calibration.
std::optional<insv::DjiClipMeta> clipMeta(const Bytes& first, const DjiModel*& model,
                                          const std::string& path) {
    const std::string product = insv::DjiProductName(first);
    model = findModel(product);
    if (model)
        return insv::ParseDjiClipMeta(first, model->schema());
    for (const auto schema : {&insv::DjiSchemaEagle4, &insv::DjiSchemaOq101}) {
        auto meta = insv::ParseDjiClipMeta(first, schema());
        if (meta && meta->calibrated) {
            std::fprintf(stderr, "[camera] %s: \"%s\" isn't a DJI product we have measured; its "
                                 "metadata read by %s, the lenses placed as the Avata 360's\n",
                         path.c_str(), product.c_str(), schema().name);
            model = &kUnknown;
            return meta;
        }
    }
    model = &kUnknown;
    return insv::ParseDjiClipMeta(first, insv::DjiSchemaEagle4());
}

// The clip's frame metadata, every sample: from the proxy when it's there
// (the same samples at its own rate, 6 % of the clip's size), else the clip.
std::vector<insv::DjiFrameMeta> frames(const ClipMetadata& clip) {
    std::vector<insv::DjiFrameMeta> out;
    for (const std::string& file : {clip.proxyPath, clip.path}) {
        if (file.empty())
            continue;
        Mp4 mp4;
        if (!mp4.open(file) || mp4.djmd < 0)
            continue;
        for (const Bytes& s : mp4.samples(0))
            if (const auto f = insv::ParseDjiFrameMeta(s))
                out.push_back(*f);
        if (!out.empty())
            return out;
    }
    return out;
}

// The GPS track in video time from the frames: a fix wherever the drone's
// part changes (10 Hz on the Avata 360), at the first frame that has it.
insv::GpsTrack gpsTrack(const std::vector<insv::DjiFrameMeta>& f, uint64_t firstFrameUs) {
    insv::GpsTrack t;
    const insv::DjiFrameMeta* last = nullptr;
    for (const insv::DjiFrameMeta& m : f) {
        if (!m.hasDrone || !m.hasPosition)
            continue;
        if (last && last->latitude == m.latitude && last->longitude == m.longitude &&
            last->velocityN == m.velocityN && last->velocityE == m.velocityE &&
            last->altitudeM == m.altitudeM)
            continue;
        last = &m;
        insv::GpsFix x;
        x.timeMs = static_cast<int64_t>(
            std::llround((static_cast<double>(m.timestampUs) - static_cast<double>(firstFrameUs)) /
                         1000.0));
        x.latitude = m.latitude;
        x.longitude = m.longitude;
        x.altitudeM = m.altitudeM;
        x.speedMps = std::hypot(m.velocityN, m.velocityE);
        x.headingDeg = x.speedMps > 0.5
                           ? std::fmod(std::atan2(m.velocityE, m.velocityN) / kDeg + 360.0, 360.0)
                           : std::nan("");
        t.fixes.push_back(x);
    }
    return t;
}

// The first sample of the clip's djmd track, by which the clip is claimed.
std::optional<Bytes> firstSample(const std::string& path, bool* djiFile = nullptr) {
    Mp4 mp4;
    if (!mp4.open(path))
        return std::nullopt;
    const bool dji = mp4.djmd >= 0 && mp4.tag("encoder").rfind("DJI", 0) == 0;
    if (djiFile)
        *djiFile = dji;
    if (!dji)
        return std::nullopt;
    auto s = mp4.samples(1);
    if (s.empty())
        return std::nullopt;
    return std::move(s.front());
}

} // namespace

CameraReader::Result DjiReader::read(const std::vector<insv::InsvStreamInfo>& streams,
                                     ClipMetadata& out, std::string& err) const {
    bool dji = false;
    const auto first = firstSample(out.path, &dji);
    if (!dji)
        return Result::NotMine;
    if (!first) {
        err = "a DJI file without metadata in its djmd track";
        return Result::Failed;
    }
    const DjiModel* model = nullptr;
    const auto meta = clipMeta(*first, model, out.path);
    if (!meta) {
        err = "the first sample of the DJI metadata has no clip metadata";
        return Result::Failed;
    }
    if (streams.size() != 2) {
        err = "a DJI file with " + std::to_string(streams.size()) +
              " lens streams; a 360 clip has two";
        return Result::Failed;
    }
    out.maker = "DJI";
    out.model = model->name[0] ? model->name : meta->productName;
    out.firmware = meta->firmware;
    out.serial = meta->productSerial;
    out.projection = Projection::DualFisheye;

    // The master lens is lens 0, wherever its stream is.
    const int master = model->masterTrack, slave = 1 - model->masterTrack;
    if (meta->calibrated) {
        out.lenses = {lens(meta->master, 0, {out.path, master}),
                      lens(meta->slave, 1, {out.path, slave})};
        out.calibrated = true;
        if (meta->slot != insv::DjiSlot::NativeRefineFar)
            std::fprintf(stderr, "[camera] %s: no %s calibration; stitching with the %s one\n",
                         out.path.c_str(), insv::DjiSlotName(insv::DjiSlot::NativeRefineFar),
                         insv::DjiSlotName(meta->slot));
        if (meta->master.width != meta->master.height ||
            std::abs(meta->master.width / out.lensWidth - std::round(meta->master.width /
                                                                     out.lensWidth)) > 1e-6)
            std::fprintf(stderr, "[camera] %s: the calibration's %gx%g frame isn't the %dx%d "
                                 "stream's at a whole scale; taken as the whole frame scaled\n",
                         out.path.c_str(), meta->master.width, meta->master.height,
                         out.lensWidth, out.lensHeight);
    } else {
        out.lenses.resize(2);
        out.lenses[0].source = {out.path, master};
        out.lenses[1].source = {out.path, slave};
        std::fprintf(stderr, "[camera] %s: no lens calibration in its metadata\n",
                     out.path.c_str());
    }
    out.mountYawDeg = model->mountDeg[0];
    out.mountPitchDeg = model->mountDeg[1];
    out.mountRollDeg = model->mountDeg[2];
    // No lens profile in the file: the bare lens's field of view is ours.
    out.bareLens = insv::LensProfile{"bare", {0, 0, 0, 0, 0, 0}, model->fovDeg};

    if (meta->hasColorMode) {
        out.colorProfile = insv::DjiColorModeName(meta->colorMode);
        if (out.colorProfile.empty())
            out.colorProfile = "colour mode " + std::to_string(meta->colorMode);
        out.logProfile = insv::DjiLogProfile(meta->colorMode);
    }
    if (meta->lroNsPerLine > 0 && meta->sensorHeight > 0)
        out.rollingShutterMs = meta->lroNsPerLine * 1e-6 * meta->sensorHeight;

    const fs::path self(out.path);
    const std::optional<std::string> held = insv::ReadDjiHeldName(out.path);
    out.proxyPath = sibling(self, self.stem().string(), ".LRF");
    if (out.proxyPath.empty() && held)
        out.proxyPath = sibling(self, fs::path(*held).stem().string(), ".LRF");
    out.recordedLocal = held ? nameTime(*held) : std::string();
    if (out.recordedLocal.empty())
        out.recordedLocal = nameTime(self.filename().string());

    // The drone's fused attitude, the only motion the file records, isn't
    // read yet (DjiReader::readImu).
    out.motionNotRead = true;
    // The GPS facts the Media information panel lists.
    const insv::GpsTrack gps = gpsTrack(frames(out), meta->clipTimestampUs);
    out.gpsRecords = out.gpsFixes = static_cast<int64_t>(gps.fixes.size());
    if (gps.fixes.size() >= 2) {
        const double sec =
            static_cast<double>(gps.fixes.back().timeMs - gps.fixes.front().timeMs) / 1000.0;
        out.gpsTrackSec = sec;
        if (sec > 0)
            out.gpsRateHz = static_cast<double>(gps.fixes.size() - 1) / sec;
    }
    return Result::Read;
}

std::optional<insv::ImuTrack> DjiReader::readImu(const ClipMetadata&) const {
    // The file has DJI's fused attitude only (no raw gyro); stabilizing from
    // it waits for a clip that turns to check it on.
    return std::nullopt;
}

std::optional<insv::GpsTrack> DjiReader::readGps(const ClipMetadata& clip) const {
    const auto first = firstSample(clip.path);
    if (!first)
        return std::nullopt;
    const DjiModel* model = nullptr;
    const auto meta = clipMeta(*first, model, clip.path);
    if (!meta)
        return std::nullopt;
    insv::GpsTrack t = gpsTrack(frames(clip), meta->clipTimestampUs);
    if (t.fixes.empty())
        return std::nullopt;
    return t;
}

std::optional<Thumbnail> DjiReader::readThumbnail(const std::string& path) const {
    Mp4 mp4;
    if (!mp4.open(path) || mp4.djmd < 0)
        return std::nullopt;
    for (unsigned i = 0; i < mp4.fmt->nb_streams; ++i) {
        const AVStream* st = mp4.fmt->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->attached_pic.size <= 0)
            continue;
        const char* codec = avcodec_get_name(st->codecpar->codec_id);
        const Bytes pic(st->attached_pic.data, st->attached_pic.data + st->attached_pic.size);
        insv::Image rgb;
        std::string err;
        if (!insv::DecodeStillPicture(pic, codec, 1024, rgb, err)) {
            std::fprintf(stderr, "[camera] the cover picture of %s doesn't decode: %s\n",
                         path.c_str(), err.c_str());
            return std::nullopt;
        }
        // An already reframed view, not the sphere.
        Thumbnail t = ThumbnailFromRgb(rgb);
        t.flat = true;
        return t;
    }
    return std::nullopt;
}

std::optional<std::string> DjiReader::clipFileFor(const std::string& path) const {
    const fs::path p(path);
    // The proxy; the .SRT isn't a file the app opens.
    if (lower(p.extension().string()) != ".lrf")
        return std::nullopt;
    std::string stem = p.stem().string();
    if (const auto held = insv::ReadDjiHeldName(path))
        stem = fs::path(*held).stem().string();
    if (stem.compare(0, 4, "DJI_") != 0)
        return std::nullopt;
    const std::string osv = sibling(p, stem, ".OSV");
    return osv.empty() ? (p.parent_path() / (stem + ".OSV")).string() : osv;
}

} // namespace camera
