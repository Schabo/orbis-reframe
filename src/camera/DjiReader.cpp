#include "DjiReader.h"

#include "ReaderUtil.h"

#include <orbis-core/dji/DjiMeta.h>
#include <orbis-core/stitch/FisheyeProjection.h> // orbiscore::Mat3, rotX / rotY / rotZ
#include <orbis-core/media/StillPicture.h>

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
    struct Mode {
        int lensWidth;
        const char* name;
    };

    const char* productName; // as clip_meta_header names it
    const char* name;        // as we show it
    const orbiscore::DjiSchema& (*schema)();
    int masterTrack; // the lens stream that holds the master lens
    // ClipMetadata::mountYawDeg / mountPitchDeg / mountRollDeg.
    double mountDeg[3];
    // The usable field of view, degrees (orbiscore::StitchParams::fovDeg).
    double fovDeg;
    // Where the master lens's picture and the other's show the product's
    // own body (orbiscore::LensMask::clearDeg, kMaskSamples each); null for none.
    const double* mask[2];
    // The fused attitude's IMU axes in the sphere's (orbiscore::ImuTrack::axisMap),
    // measured against the picture's own rotation (docs/formats/dji-360.md §4).
    const char* imuAxes;
    // Lens stream sizes (square) our clips show to be the whole calibration
    // frame scaled, though not by a whole factor; 0-terminated, null for none.
    // Another size off a whole factor might be a crop, so it is logged.
    const int* scaledSizes;
    // The product's names for its 360 video modes by lens stream size
    // (ClipMetadata::captureMode); 0-terminated, null for none.
    const Mode* modes;
};

// The Avata 360's 3000 px modes: the whole 3840 px frame at 0.78125, by the
// image circle's place in the frame and by the seam (docs/formats/dji-360.md
// §3).
const int kAvata360ScaledSizes[] = {3000, 0};

// The Avata 360's 360 video modes as DJI names them (its spec page; 4K is
// the 100 fps mode a firmware update added), by their lens stream sizes on
// our clips (docs/formats/dji-360.md, *Modes and sample clips*). The file
// names no mode itself.
const DjiModel::Mode kAvata360Modes[] = {{3840, "8K"}, {3000, "6K"}, {1920, "4K"}, {0, nullptr}};

constexpr int kMaskSamples = 180; // every 2 degrees round the picture

// Where the Avata 360's body shows in each lens's picture (the rear duct and
// its arms, up to ~91 deg off the axis -- inside the overlap behind the
// drone), as orbiscore::LensMask::clearDeg every 2 degrees: measured from our six
// clips, the body being what stays put in a lens's picture while the scene
// moves past (docs/formats/dji-360.md §3.2), drawn half a degree in and over
// the next 2 degrees either side. The file's own occlusion polygon
// (occlusion_pt_x/y) is zero on the Avata 360.
const double kAvata360MasterMask[kMaskSamples] = {
    93.9, 95.1, 95.9, 96.1, 96.1, 96.4, 96.5, 96.6, 97.0, 97.4, 97.9, 98.4,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 97.2, 96.9, 96.6, 96.3, 96.3, 96.0, 95.9, 95.7,
    95.6, 95.5, 95.4, 95.4, 95.4, 95.4, 95.4, 94.9, 94.3, 93.9, 93.5, 93.3,
    93.1, 92.9, 92.5, 92.5, 92.6, 92.6, 92.7, 92.9, 93.1, 93.3, 93.6, 93.9,
    94.2, 94.0, 93.9, 93.8, 93.8, 93.8, 93.8, 93.8, 93.8, 93.9, 93.9, 94.1,
    94.1, 94.2, 94.4, 94.7, 95.2, 95.2, 95.8, 96.9, 97.3, 97.3, 97.2, 97.0,
    96.9, 96.8, 96.6, 96.4, 96.3, 96.3, 95.7, 95.7, 93.3, 93.3, 93.0, 93.0,
};
const double kAvata360SlaveMask[kMaskSamples] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 98.1, 97.8, 97.6, 97.4, 97.3, 97.2,
    96.9, 96.3, 96.3, 96.3, 96.9, 97.1, 97.8, 98.2, 98.6, 99.0, 97.2, 97.2,
    97.2, 97.2, 97.0, 97.0, 94.3, 94.3, 94.3, 94.3, 94.2, 93.5, 93.5, 93.5,
    93.6, 93.6, 94.2, 94.6, 94.1, 93.8, 93.4, 93.0, 92.6, 92.5, 92.4, 92.0,
    92.0, 92.1, 92.1, 91.9, 91.5, 91.2, 91.2, 92.3, 92.4, 92.8, 92.9, 93.3,
    93.7, 93.7, 95.6, 95.4, 95.2, 94.4, 94.3, 94.3, 94.3, 94.3, 95.1, 95.2,
    95.3, 95.4, 95.4, 95.4, 96.2, 98.3, 0, 0, 0, 0, 0, 0,
    0, 98.8, 98.4, 98.1, 98.0, 98.0, 98.5, 98.8, 98.9, 0, 0, 0,
};

// The Avata 360 flies its camera with the lens axes vertical, the master
// lens up, and the drone's nose a quarter turn from the master picture's
// top: the mount turns the master's axis to the sphere's up and the nose to
// its forward. The nose measured on our clip from the sun against the
// drone's own attitude (docs/formats/dji-360.md §3); the picture reaches
// ~100 deg off each lens's axis before the frame's edge and the housing.
const DjiModel kModels[] = {
    {"DJI Avata360", "DJI Avata 360", &orbiscore::DjiSchemaEagle4, 0, {90.0, -90.0, 90.0}, 200.0,
     {kAvata360MasterMask, kAvata360SlaveMask}, "y,x,-z", kAvata360ScaledSizes, kAvata360Modes},
};

// For a product not in the table: the schema tried in turn, the master on
// the first track, the lens axes as the Avata 360's.
const DjiModel kUnknown = {"", "", &orbiscore::DjiSchemaEagle4, 0, {90.0, -90.0, 90.0}, 200.0,
                           {nullptr, nullptr}, "y,x,-z", nullptr, nullptr};

// Whether `size` is one of `model`'s measured whole-frame scales.
bool knownScale(const DjiModel& model, int size) {
    for (const int* s = model.scaledSizes; s && *s; ++s)
        if (*s == size)
            return true;
    return false;
}

// `model`'s name for the mode filmed at `lensWidth`, or "" when it has none.
const char* modeName(const DjiModel& model, int lensWidth) {
    for (const DjiModel::Mode* m = model.modes; m && m->name; ++m)
        if (m->lensWidth == lensWidth)
            return m->name;
    return "";
}

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
// docs/formats/dji-360.md §3) in the stitch's terms (orbiscore::LensPose): its
// lens frame is the image's turned half a turn about the optical axis, so
// lens -> rig is that turn on both sides of the transposed rotation. The
// whole pose goes into the mount; the calibration's own angles stay 0.
orbiscore::LensPose lensPose(const orbiscore::DjiDewarp& d) {
    const orbiscore::Mat3 camFromRig =
        orbiscore::mul(orbiscore::mul(orbiscore::rotY(d.yaw * kDeg),
                                      orbiscore::rotZ(d.roll * kDeg)),
                       orbiscore::rotX((d.pitch - 90.0) * kDeg));
    const orbiscore::Mat3 flip = orbiscore::rotZ(std::numbers::pi);
    const orbiscore::Mat3 m =
        orbiscore::mul(orbiscore::mul(flip, orbiscore::transpose(camFromRig)), flip);
    orbiscore::LensPose p;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            p.mount[r * 3 + c] = m.m[r][c];
    p.rollZeroDeg = 0.0;
    return p;
}

// A lens as the clip description takes it: the theta-polynomial in the
// calibration's fields, its frame as lens `index`'s half of a side-by-side
// frame of two (orbiscore::LensCalibExt's convention), the fifth term beside.
Lens lens(const orbiscore::DjiDewarp& d, int index, const LensSource& source) {
    Lens l;
    l.source = source;
    l.model = orbiscore::LensModel::ThetaPoly;
    orbiscore::LensCalibExt& e = l.calibration;
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
std::optional<orbiscore::DjiClipMeta> clipMeta(const Bytes& first, const DjiModel*& model,
                                               const std::string& path) {
    const std::string product = orbiscore::DjiProductName(first);
    model = findModel(product);
    if (model)
        return orbiscore::ParseDjiClipMeta(first, model->schema());
    for (const auto schema : {&orbiscore::DjiSchemaEagle4, &orbiscore::DjiSchemaOq101}) {
        auto meta = orbiscore::ParseDjiClipMeta(first, schema());
        if (meta && meta->calibrated) {
            std::fprintf(stderr, "[camera] %s: \"%s\" isn't a DJI product we have measured; its "
                                 "metadata read by %s, the lenses placed as the Avata 360's\n",
                         path.c_str(), product.c_str(), schema().name);
            model = &kUnknown;
            return meta;
        }
    }
    model = &kUnknown;
    return orbiscore::ParseDjiClipMeta(first, orbiscore::DjiSchemaEagle4());
}

// The clip's frame metadata, every sample, from the clip itself. Not from
// the proxy: it keeps one frame's sample in four (docs/formats/dji-360.md
// §1), and reading only the djmd track of the clip costs about the same.
std::vector<orbiscore::DjiFrameMeta> frames(const ClipMetadata& clip) {
    std::vector<orbiscore::DjiFrameMeta> out;
    Mp4 mp4;
    if (!mp4.open(clip.path) || mp4.djmd < 0)
        return out;
    for (const Bytes& s : mp4.samples(0))
        if (const auto f = orbiscore::ParseDjiFrameMeta(s))
            out.push_back(*f);
    return out;
}

// The GPS track in video time from the frames: a fix wherever the drone's
// part changes (10 Hz on the Avata 360), at the first frame that has it.
orbiscore::GpsTrack gpsTrack(const std::vector<orbiscore::DjiFrameMeta>& f, uint64_t firstFrameUs) {
    orbiscore::GpsTrack t;
    const orbiscore::DjiFrameMeta* last = nullptr;
    for (const orbiscore::DjiFrameMeta& m : f) {
        if (!m.hasDrone || !m.hasPosition)
            continue;
        if (last && last->latitude == m.latitude && last->longitude == m.longitude &&
            last->velocityN == m.velocityN && last->velocityE == m.velocityE &&
            last->altitudeM == m.altitudeM)
            continue;
        last = &m;
        orbiscore::GpsFix x;
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
// The sensor's readout, seconds: DJI's line time over all its lines (the
// stream is the whole frame scaled), 9.4 ms on the Avata 360 at 100 fps,
// 16.2 ms in its 24-60 fps modes; 0 when the file doesn't say.
double readoutSec(const orbiscore::DjiClipMeta& meta) {
    return static_cast<double>(meta.lroNsPerLine) * 1e-9 * static_cast<double>(meta.sensorHeight);
}

// The camera's fused attitude as an orientation track: 4 kHz on the Avata
// 360, the only motion its files record (no raw gyro, docs/formats/dji-360.md
// §4). Each frame's batch is stamped with its first sample's time on a 32-bit
// device clock, so it is put back on the frame's 64-bit one by their small
// difference; the samples are timed by a straight line of stamp against
// sample count over the neighbouring batches (a stamp alone sits on a 250 us
// grid). Times stay on the frames' clock, device us. Each frame is taken at
// the middle of its readout, half of `readoutSec` after its own stamp,
// which is where the picture's rotation lines up with the attitude's
// (measured 4.4-5.8 ms on seven 100 fps clips, against 4.7; 7.4-9.7 ms on
// five at 30 and 60 fps, against 8.1). nullopt with fewer than two batches.
std::optional<orbiscore::ImuTrack> attitudeTrack(const std::vector<orbiscore::DjiFrameMeta>& frames,
                                                 const orbiscore::DjiClipMeta& meta,
                                                 const DjiModel& model, double fps,
                                                 double readoutSec) {
    struct Batch {
        double stampUs;  // first sample, on the frames' clock
        double index;    // its count among all samples
        const orbiscore::DjiFrameMeta* frame;
    };
    std::vector<Batch> batches;
    size_t total = 0;
    for (const orbiscore::DjiFrameMeta& m : frames) {
        if (m.attitude.empty())
            continue;
        const auto behind = static_cast<int32_t>(static_cast<uint32_t>(m.timestampUs) -
                                                 static_cast<uint32_t>(m.attitudeUs));
        batches.push_back({static_cast<double>(m.timestampUs) - behind, static_cast<double>(total),
                           &m});
        total += m.attitude.size();
    }
    if (batches.size() < 2 || total < 2 || !(fps > 0.0))
        return std::nullopt;

    // The line over +-kHalf batches, by running sums; a slope far from the
    // stated rate (a batch count out of step with its stamps) is replaced by it.
    constexpr size_t kHalf = 50;
    const double statedUs = meta.imuRateHz > 0 ? 1e6 / meta.imuRateHz : 0.0;
    const size_t n = batches.size();
    std::vector<double> sx(n + 1, 0.0), sy(n + 1, 0.0), sxx(n + 1, 0.0), sxy(n + 1, 0.0);
    for (size_t k = 0; k < n; ++k) {
        const double x = batches[k].index, y = batches[k].stampUs - batches[0].stampUs;
        sx[k + 1] = sx[k] + x;
        sy[k + 1] = sy[k] + y;
        sxx[k + 1] = sxx[k] + x * x;
        sxy[k + 1] = sxy[k] + x * y;
    }
    size_t offRate = 0;
    const auto lineAt = [&](size_t k, double& a, double& b) {
        const size_t lo = k > kHalf ? k - kHalf : 0, hi = std::min(n, k + kHalf + 1);
        const double c = static_cast<double>(hi - lo);
        const double mx = (sx[hi] - sx[lo]) / c, my = (sy[hi] - sy[lo]) / c;
        const double vxx = (sxx[hi] - sxx[lo]) / c - mx * mx;
        b = vxx > 0.0 ? ((sxy[hi] - sxy[lo]) / c - mx * my) / vxx : statedUs;
        if (statedUs > 0.0 && std::abs(b - statedUs) > 0.05 * statedUs) {
            b = statedUs;
            ++offRate;
        }
        a = my - b * mx;
    };

    orbiscore::ImuTrack track;
    track.axisMap = model.imuAxes;
    track.orientation.reserve(total);
    const double t0 = batches[0].stampUs;
    for (size_t k = 0; k < n; ++k) {
        double a = 0.0, b = 0.0;
        lineAt(k, a, b);
        const std::vector<std::array<float, 4>>& q = batches[k].frame->attitude;
        for (size_t i = 0; i < q.size(); ++i) {
            const double w = q[i][0], x = q[i][1], y = q[i][2], z = q[i][3];
            const double norm = std::sqrt(w * w + x * x + y * y + z * z);
            if (norm < 0.5)
                continue;
            // World <- IMU, the world NED: world up (-z) and north (+x) in the
            // IMU's axes are the third row negated and the first row.
            const double s2 = 2.0 / (norm * norm);
            orbiscore::OrientationSample o;
            const double tUs = t0 + a + b * (batches[k].index + static_cast<double>(i));
            o.timestamp = static_cast<uint64_t>(std::llround(tUs));
            o.up[0] = -(s2 * (x * z - w * y));
            o.up[1] = -(s2 * (y * z + w * x));
            o.up[2] = -(1.0 - s2 * (x * x + y * y));
            o.north[0] = 1.0 - s2 * (y * y + z * z);
            o.north[1] = s2 * (x * y - w * z);
            o.north[2] = s2 * (x * z + w * y);
            track.orientation.push_back(o);
        }
    }
    if (offRate)
        std::fprintf(stderr, "[camera] DJI attitude: %zu of %zu batches out of step with their "
                             "stamps; timed at the stated %u Hz there\n",
                     offRate, n, meta.imuRateHz);
    if (track.orientation.size() < 2)
        return std::nullopt;

    // Video frame k is the djmd track's sample k, both at the stream's rate;
    // track time counts from the earliest sample.
    const auto firstUs = static_cast<double>(track.orientation.front().timestamp);
    track.frameTimes.reserve(frames.size());
    for (size_t k = 0; k < frames.size(); ++k)
        track.frameTimes.push_back(
            {static_cast<double>(k) / fps,
             (static_cast<double>(frames[k].timestampUs) - firstUs) * 1e-6 + 0.5 * readoutSec});
    track.hasOffset = true;
    track.offsetSec = track.frameTimes.front().trackSec;
    track.readoutSec = readoutSec;
    return track;
}

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

CameraReader::Result DjiReader::read(const std::vector<orbiscore::InsvStreamInfo>& streams,
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
    if (out.lensWidth == out.lensHeight)
        out.captureMode = modeName(*model, out.lensWidth);

    // The master lens is lens 0, wherever its stream is.
    const int master = model->masterTrack, slave = 1 - model->masterTrack;
    if (meta->calibrated) {
        out.lenses = {lens(meta->master, 0, {out.path, master}),
                      lens(meta->slave, 1, {out.path, slave})};
        for (int i = 0; i < 2; ++i)
            if (model->mask[i])
                out.lenses[i].mask.clearDeg.assign(model->mask[i], model->mask[i] + kMaskSamples);
        out.calibrated = true;
        if (meta->slot != orbiscore::DjiSlot::NativeRefineFar)
            std::fprintf(stderr, "[camera] %s: no %s calibration; stitching with the %s one\n",
                         out.path.c_str(),
                         orbiscore::DjiSlotName(orbiscore::DjiSlot::NativeRefineFar),
                         orbiscore::DjiSlotName(meta->slot));
        const bool wholeScale =
            std::abs(meta->master.width / out.lensWidth -
                     std::round(meta->master.width / out.lensWidth)) <= 1e-6;
        if (meta->master.width != meta->master.height ||
            (!wholeScale && !(out.lensWidth == out.lensHeight && knownScale(*model, out.lensWidth))))
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
    out.bareLens = orbiscore::LensProfile{"bare", {0, 0, 0, 0, 0, 0}, model->fovDeg};

    if (meta->hasColorMode) {
        out.colorProfile = orbiscore::DjiColorModeName(meta->colorMode);
        if (out.colorProfile.empty())
            out.colorProfile = "colour mode " + std::to_string(meta->colorMode);
        out.logProfile = orbiscore::DjiLogProfile(meta->colorMode);
    }
    out.rollingShutterMs = readoutSec(*meta) * 1e3;

    const fs::path self(out.path);
    const std::optional<std::string> held = orbiscore::ReadDjiHeldName(out.path);
    out.proxyPath = sibling(self, self.stem().string(), ".LRF");
    if (out.proxyPath.empty() && held)
        out.proxyPath = sibling(self, fs::path(*held).stem().string(), ".LRF");
    out.recordedLocal = held ? nameTime(*held) : std::string();
    if (out.recordedLocal.empty())
        out.recordedLocal = nameTime(self.filename().string());

    const std::vector<orbiscore::DjiFrameMeta> all = frames(out);
    // The motion: the camera's fused attitude (DjiReader::readImu), at its
    // rate over the clip.
    size_t samples = 0;
    for (const orbiscore::DjiFrameMeta& m : all)
        samples += m.attitude.size();
    if (samples >= 2 && all.size() >= 2 && all.back().timestampUs > all.front().timestampUs) {
        out.hasMotion = true;
        out.imuRateHz = static_cast<double>(samples) * 1e6 /
                        (static_cast<double>(all.back().timestampUs - all.front().timestampUs) +
                         1e6 / std::max(out.fps, 1.0));
    }
    // The GPS facts the Media information panel lists.
    const orbiscore::GpsTrack gps = gpsTrack(all, meta->clipTimestampUs);
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

std::optional<orbiscore::ImuTrack> DjiReader::readImu(const ClipMetadata& clip) const {
    const auto first = firstSample(clip.path);
    if (!first)
        return std::nullopt;
    const DjiModel* model = nullptr;
    const auto meta = clipMeta(*first, model, clip.path);
    if (!meta)
        return std::nullopt;
    return attitudeTrack(frames(clip), *meta, *model, clip.fps, readoutSec(*meta));
}

std::optional<orbiscore::GpsTrack> DjiReader::readGps(const ClipMetadata& clip) const {
    const auto first = firstSample(clip.path);
    if (!first)
        return std::nullopt;
    const DjiModel* model = nullptr;
    const auto meta = clipMeta(*first, model, clip.path);
    if (!meta)
        return std::nullopt;
    orbiscore::GpsTrack t = gpsTrack(frames(clip), meta->clipTimestampUs);
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
        orbiscore::Image rgb;
        std::string err;
        if (!orbiscore::DecodeStillPicture(pic, codec, 1024, rgb, err)) {
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
    if (const auto held = orbiscore::ReadDjiHeldName(path))
        stem = fs::path(*held).stem().string();
    if (stem.compare(0, 4, "DJI_") != 0)
        return std::nullopt;
    const std::string osv = sibling(p, stem, ".OSV");
    return osv.empty() ? (p.parent_path() / (stem + ".OSV")).string() : osv;
}

} // namespace camera
