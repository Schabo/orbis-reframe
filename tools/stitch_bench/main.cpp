// stitch_bench -- a stitcher's sphere over a stretch of a clip, measured on
// the picture alone and against a reference video of the same stretch
// (Insta360 Studio's export), so any stitcher -- the app's, a new one, the
// reference itself -- is scored the same way.
//
//   stitch_bench LIST --out DIR [--baseline DIR] [--only TEXT] [--width N]
//                [--every N] [--video-px-deg N] [--no-video] [--live] [--fit-time]
//
// LIST has one stretch per line, tab-separated: a name (the output files'),
// the clip, the start and end in seconds, then any of
//   stitcher=current|video:FILE   what is measured (default current: the
//                                  app's NativeStitchSource; video: a stitched
//                                  equirect video lined up as the reference)
//   guard=none|standard|premium   (default premium, as the app)
//   method=depth|fixed|new         the stitcher (current; default depth):
//                                  the old one with the depth-aware seam or
//                                  a fixed one, or the new one (which takes
//                                  no seam correction or lens colour)
//   depth=on|off                   the same as method=depth / fixed
//   colourblend=on|off             the new stitcher's seam colour blend
//                                  (method=new; default on, as the app)
//   rs=on|off                      the capture-time (rolling shutter)
//                                  correction from the gyro (current;
//                                  default on, as playback)
//   seam=Y,P,R,CU,CV,S             the clip's seam correction (current;
//                                  measured over the clip when not given)
//   colour=R,G,B,SR,SG,SB,SPREAD   its lens colour (likewise)
//   calibration=FILE.json          the camera's own lens calibration (as
//                                  the app keeps it, tools/lens_calib's
//                                  --out) in place of the file's (current)
//   ref=FILE|template              the reference: a stitched equirect video
//                                  (Studio's export), or our own stitch from
//                                  the file's data alone, whose geometry
//                                  holds still (for clips without one)
//   refstart=S                     clip time of the reference video's first
//                                  frame
//   refrot=Z,Y,X                   the rotation from our sphere to the
//                                  reference video's (as seam_regress)
// and anything after a '#' is a note.
//
// Every frame of the stretch is measured on its seam strip (the seam circle
// unwrapped, +-30 deg across it at 20 px/deg): the sharpness against the
// reference's, and how much the offset between ours and the reference along
// the seam changes from the frame before ("alive": a stitch whose geometry
// holds still keeps it, whatever passes through). Every --every'th frame
// also the whole sphere at --width: PSNR (raw and colour-matched), and the
// geometry -- where ours puts the reference's content, by region and per
// lens by off-axis angle, and the rotation left between them. The band and
// the lenses' axes come from the file's calibration (guard included), never
// from the stitcher, so every stitcher of a clip is measured on the same
// grid. Written to DIR: NAME.json (summary for --baseline, per-frame
// series, profiles) and NAME_strip.y4m, the strips +-12 deg as a video
// (rows: ours, the reference, |ours colour-matched - reference| x 4).

#include "Metrics.h"

#include <LensCalibrationJson.h>

#include <ClipLensColour.h>
#include <ClipMeasure.h>
#include <ClipSeam.h>
#include <ClipSeamAnalysis.h>
#include <FlatVideoReader.h>
#include <NativeStitchSource.h>

#include <FisheyeProjection.h>
#include <GyroStabilizer.h>

#include <ClipMetadata.h> // camera::ReadClipImu

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;
using LensGuard = stitch::PreviewStitchConfig::LensGuard;
using Method = stitch::PreviewStitchConfig::Method;

const char kUsage[] =
    "usage: stitch_bench LIST --out DIR [--baseline DIR] [--only TEXT] [--width N]\n"
    "       [--every N] [--video-px-deg N] [--no-video] [--live] [--fit-time]\n"
    "  LIST: name<TAB>clip<TAB>start<TAB>end[<TAB>stitcher=..|guard=..|method=..|rs=..|seam=..|\n"
    "        colour=..|ref=..|refstart=..|refrot=..]... [# note]\n";

struct Entry {
    std::string name, clip, note;
    int64_t startMs = 0, endMs = 0;
    std::string stitcher = "current"; // or video:FILE
    LensGuard guard = LensGuard::Premium;
    Method method = Method::DepthSeam;
    bool captureTiming = true;
    bool seamColourBlend = true;
    std::optional<insv::SeamCorrection> seam;
    std::optional<insv::LensColour> colour;
    std::optional<stitch::LensCalibration> calibration;
    std::string ref;
    double refStartMs = 0;
    double refRotDeg[3] = {0, 0, 0}; // z, y, x
};

struct Options {
    std::string outDir, baseline, only;
    int width = 3840;  // the whole-sphere measurements
    int every = 10;    // ... on every N-th frame
    double videoPxDeg = 6;
    bool video = true;
    bool live = false; // current: no seam analysis first
    bool fitTime = false;
};

bool ParseList(const std::string& path, std::vector<Entry>& out, std::string& err)
{
    std::ifstream in(path);
    if (!in) {
        err = "can't read " + path;
        return false;
    }
    std::string line;
    int no = 0;
    while (std::getline(in, line)) {
        ++no;
        const auto hash = line.find('#');
        Entry e;
        if (hash != std::string::npos) {
            e.note = line.substr(hash + 1);
            e.note.erase(0, e.note.find_first_not_of(" \t"));
            line.erase(hash);
        }
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string tok; std::getline(ss, tok, '\t');) {
            tok.erase(0, tok.find_first_not_of(' '));
            tok.erase(tok.find_last_not_of(' ') + 1);
            if (!tok.empty())
                f.push_back(tok);
        }
        if (f.empty())
            continue;
        const std::string where = path + ":" + std::to_string(no) + ": ";
        if (f.size() < 4) {
            err = where + "want name, clip, start and end, tab-separated";
            return false;
        }
        e.name = f[0];
        e.clip = f[1];
        e.startMs = std::llround(std::atof(f[2].c_str()) * 1000.0);
        e.endMs = std::llround(std::atof(f[3].c_str()) * 1000.0);
        if (e.endMs <= e.startMs) {
            err = where + "the end must be after the start";
            return false;
        }
        for (size_t k = 4; k < f.size(); ++k) {
            const auto eq = f[k].find('=');
            const std::string key = f[k].substr(0, eq);
            const std::string v = eq == std::string::npos ? "" : f[k].substr(eq + 1);
            if (key == "stitcher") {
                if (v != "current" && v.rfind("video:", 0) != 0) {
                    err = where + "stitcher= wants current or video:FILE";
                    return false;
                }
                e.stitcher = v;
            } else if (key == "guard") {
                e.guard = v == "none"       ? LensGuard::None
                          : v == "standard" ? LensGuard::Standard
                                            : LensGuard::Premium;
            } else if (key == "depth") {
                e.method = v != "off" ? Method::DepthSeam : Method::FixedSeam;
            } else if (key == "method") {
                if (v == "depth")
                    e.method = Method::DepthSeam;
                else if (v == "fixed")
                    e.method = Method::FixedSeam;
                else if (v == "new")
                    e.method = Method::New;
                else {
                    err = where + "method= wants depth, fixed or new";
                    return false;
                }
            } else if (key == "rs") {
                e.captureTiming = v != "off";
            } else if (key == "colourblend") {
                e.seamColourBlend = v != "off";
            } else if (key == "seam") {
                double a[6];
                if (std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &a[0], &a[1], &a[2], &a[3], &a[4],
                                &a[5]) != 6) {
                    err = where + "seam= wants Y,P,R,CU,CV,S";
                    return false;
                }
                insv::SeamCorrection s;
                for (int i = 0; i < 3; ++i)
                    s.lens1RotDeg[i] = a[i];
                s.lens1Centre[0] = a[3] * 0.01;
                s.lens1Centre[1] = a[4] * 0.01;
                s.radialScale = 1.0 + a[5] * 0.01;
                e.seam = s;
            } else if (key == "colour") {
                double a[7];
                if (std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf,%lf", &a[0], &a[1], &a[2], &a[3],
                                &a[4], &a[5], &a[6]) != 7) {
                    err = where + "colour= wants R,G,B,SR,SG,SB,SPREAD";
                    return false;
                }
                insv::LensColour c;
                for (int i = 0; i < 3; ++i) {
                    c.logRatio[i] = std::log1p(a[i] * 0.01);
                    c.slope[i] = a[3 + i] * 0.01;
                }
                c.spread = a[6];
                e.colour = c;
            } else if (key == "calibration") {
                std::ifstream cf(v);
                const nlohmann::json j = nlohmann::json::parse(cf, nullptr, false);
                e.calibration = lenscal::FromJson(j);
                if (!e.calibration) {
                    err = where + "calibration= can't read " + v;
                    return false;
                }
            } else if (key == "ref") {
                e.ref = v;
            } else if (key == "refstart") {
                e.refStartMs = std::atof(v.c_str()) * 1000.0;
            } else if (key == "refrot") {
                if (std::sscanf(v.c_str(), "%lf,%lf,%lf", &e.refRotDeg[0], &e.refRotDeg[1],
                                &e.refRotDeg[2]) != 3) {
                    err = where + "refrot= wants Z,Y,X";
                    return false;
                }
            } else {
                err = where + "unknown field " + f[k];
                return false;
            }
        }
        if (e.stitcher != "current" && (e.ref.empty() || e.ref == "template")) {
            err = where + "a video stitcher is lined up as the reference: give ref=FILE too";
            return false;
        }
        out.push_back(e);
    }
    return true;
}

const char* GuardName(LensGuard g)
{
    return g == LensGuard::None ? "none" : g == LensGuard::Standard ? "standard" : "premium";
}

const char* MethodName(Method m)
{
    return m == Method::DepthSeam ? "depth" : m == Method::FixedSeam ? "fixed" : "new";
}

// --- the seam frame from the file ------------------------------------------

bench::V3 ToBench(const insv::Vec3& v) { return {v.x, v.y, v.z}; }

// The band and the lenses' axes in the sphere frame, from the calibration
// and the guard alone: no seam correction, nothing measured, so every
// stitcher of the clip is measured on the same grid.
bool SeamFrameOf(const Entry& e, bench::SeamFrame& out, std::shared_ptr<const camera::ClipMetadata>& meta,
                 std::string& err)
{
    insv::InsvCalibration calib;
    insv::StitchParams params;
    if (stitch::PrepareClipMeasurement(e.clip, e.guard, nullptr, calib, params, err, &meta) !=
        stitch::MeasureFailure::None)
        return false;
    params.depthSeam = true; // for the seam frame
    insv::FisheyeStitcher st;
    if (!st.configure(calib, meta->lensWidth, meta->lensHeight, params)) {
        err = "FisheyeStitcher::configure failed";
        return false;
    }
    const insv::Mat3 toSphere = insv::transpose(st.outRotation()); // world = R * sphere
    out.axis = ToBench(insv::apply(toSphere, st.seamAxis()));
    out.u = ToBench(insv::apply(toSphere, st.seamU()));
    out.v = ToBench(insv::apply(toSphere, st.seamV()));
    for (int i = 0; i < 2; ++i) {
        const insv::Mat3& w2l = st.lensProjection(i).worldToLens; // the lens's +z is its axis
        out.lensAxis[i] = ToBench(insv::apply(toSphere, {w2l.m[2][0], w2l.m[2][1], w2l.m[2][2]}));
    }
    if (bench::Dot(out.lensAxis[0], out.axis) < 0 || bench::Dot(out.lensAxis[1], out.axis) > 0) {
        err = "the lenses' axes don't face the seam axis's two ways";
        return false;
    }
    out.bandDeg = st.gpuParams().bandRad / kDeg;
    return true;
}

// --- what is measured: a stream of stitched equirects ------------------------

struct Picture {
    cv::Mat rgba; // CV_8UC4, owns its pixels
    int64_t clipMs = 0;
    // In the reference's sphere (a Studio export), not ours: sampled through
    // refrot like the reference, so both are resampled once.
    bool refFrame = false;
};

class Stitcher {
public:
    virtual ~Stitcher() = default;
    virtual bool next(Picture& out) = 0; // false at the stretch's end or on error
    std::string error;
};

// The app's stitch: NativeStitchSource as playback runs it, the seam
// correction, lens colour and capture timing set as the app sets them.
class CurrentStitcher : public Stitcher {
public:
    bool open(const Entry& e, const Options& opt, const camera::ClipMetadata& meta, std::string& info)
    {
        endMs_ = e.endMs;
        const stitch::LensCalibration* cal = e.calibration ? &*e.calibration : nullptr;
        insv::SeamCorrection seam;
        std::string seamFrom = "list", colourFrom = "list";
        if (e.method == Method::New) {
            seamFrom = colourFrom = "not used by the new stitcher";
        } else if (e.seam) {
            seam = *e.seam;
        } else {
            const auto r = stitch::MeasureClipSeam(e.clip, 0, meta.durationMs, e.guard, cal);
            seamFrom = r.ok ? "measured" : "none (" + r.message + ")";
            if (r.ok)
                seam = r.correction;
        }
        insv::LensColour colour;
        if (e.method == Method::New) {
        } else if (e.colour) {
            colour = *e.colour;
        } else {
            const auto r =
                stitch::MeasureClipLensColour(e.clip, 0, meta.durationMs, e.guard, cal, seam);
            colourFrom = r.ok ? "measured" : "none (" + r.message + ")";
            if (r.ok)
                colour = r.colour;
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "seam=%.4f,%.4f,%.4f,%.4f,%.4f,%.4f (%s)  "
                      "colour=%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f (%s)",
                      seam.lens1RotDeg[0], seam.lens1RotDeg[1], seam.lens1RotDeg[2],
                      seam.lens1Centre[0] * 100, seam.lens1Centre[1] * 100, (seam.radialScale - 1) * 100,
                      seamFrom.c_str(), std::expm1(colour.logRatio[0]) * 100,
                      std::expm1(colour.logRatio[1]) * 100, std::expm1(colour.logRatio[2]) * 100,
                      colour.slope[0] * 100, colour.slope[1] * 100, colour.slope[2] * 100, colour.spread,
                      colourFrom.c_str());
        info = buf;
        if (cal)
            info += "\n  lens calibration " + cal->serial + " " + cal->created;

        if (e.method == Method::DepthSeam && !opt.live) {
            const stitch::SeamAnalysisResult r =
                stitch::AnalyseClipSeam({e.clip, e.guard, seam, colour, e.calibration}, e.startMs,
                                        e.endMs);
            info += "\n  seam analysis: " + r.message;
            if (!r.ok) {
                error = "seam analysis: " + r.message;
                return false;
            }
        }

        stitch::PreviewStitchConfig config;
        config.width = stitch::FullSphereWidth(meta.lensWidth);
        config.height = config.width / 2;
        config.lensGuard = e.guard;
        config.method = e.method;
        config.seamColourBlend = e.seamColourBlend;
        config.lensCalibration = e.calibration;
        if (!src_.Open(e.clip, config)) {
            error = "open: " + src_.GetLastError();
            return false;
        }
        src_.SetSeamCorrection(seam);
        src_.SetLensColour(colour);
        // The capture-time correction, from the gyro as playback has it
        // (the clip's own sync, 0, as a clip without a measured one).
        stab_ = std::make_shared<insv::GyroStabilizer>();
        if (!e.captureTiming)
            info += "\n  (rs=off: no capture-time correction)";
        else if (const auto imu = camera::ReadClipImu(e.clip))
            stab_->build(*imu, insv::StabConfig::preset(insv::StabMode::HorizonLock), 0.0);
        if (stab_->ready()) {
            stitch::CaptureTiming t;
            t.readoutSec[0] = t.readoutSec[1] = stab_->readoutSec();
            auto s = stab_;
            t.rotationAt = [s](double tSec, double dtSec, float r[3]) { s->bodyRotation(tSec, dtSec, r); };
            src_.SetCaptureTiming(std::move(t));
        } else if (e.captureTiming) {
            info += "\n  (no gyro: no capture-time correction)";
        }
        if (!src_.GpuViewAvailable())
            info += "\n  (no GPU stitch: the CPU fallback is measured)";
        if (!src_.SeekTo(e.startMs)) {
            error = "seek: " + src_.GetLastError();
            return false;
        }
        return true;
    }

    bool next(Picture& out) override
    {
        const stitch::Frame f = src_.ReadFrame();
        if (f.rgba.empty() || f.timestamp_ms > endMs_)
            return false;
        out.clipMs = f.timestamp_ms;
        out.rgba = cv::Mat(f.height, f.width, CV_8UC4, const_cast<uint8_t*>(f.rgba.data())).clone();
        return true;
    }

private:
    stitch::NativeStitchSource src_;
    std::shared_ptr<insv::GyroStabilizer> stab_;
    int64_t endMs_ = 0;
};

// A stitched equirect video (another Studio mode), on the reference's
// timing (its frame at clip time refStart + pts) and in its sphere.
class VideoStitcher : public Stitcher {
public:
    bool open(const std::string& path, const Entry& e)
    {
        endMs_ = e.endMs;
        refStartMs_ = e.refStartMs;
        if (!reader_.open(path, error))
            return false;
        if (!reader_.seek(std::max<int64_t>(0, std::llround(e.startMs - e.refStartMs)))) {
            error = reader_.lastError();
            return false;
        }
        startMs_ = e.startMs;
        return true;
    }

    bool next(Picture& out) override
    {
        exportpipe::VideoFrame f;
        while (reader_.readNext(f)) {
            const double clipMs = double(f.ptsMs) + refStartMs_;
            if (clipMs > double(endMs_))
                return false;
            if (clipMs + 0.5 < double(startMs_))
                continue; // seek lands at or before
            out.clipMs = std::llround(clipMs);
            out.rgba = cv::Mat(f.height, f.width, CV_8UC4, f.rgba.data()).clone();
            out.refFrame = true;
            return true;
        }
        return false;
    }

private:
    exportpipe::FlatVideoReader reader_;
    int64_t endMs_ = 0, startMs_ = 0;
    double refStartMs_ = 0;
};

// The picture each frame of ours is compared with.
class Reference {
public:
    virtual ~Reference() = default;
    // The reference at a clip time (RGBA, valid until the next call), empty
    // if it has none there.
    virtual cv::Mat at(int64_t clipMs) = 0;
    // In the reference's own sphere, turned into ours by refrot.
    virtual bool refFrame() const = 0;
};

// A stitched equirect video (Studio's export): its frame nearest a clip
// time, the first at clip time refstart.
class VideoReference : public Reference {
public:
    bool open(const Entry& e, std::string& err)
    {
        if (!reader_.open(e.ref, err))
            return false;
        frameMs_ = reader_.info().fps > 0 ? 1000.0 / reader_.info().fps : 33.37;
        startMs_ = e.refStartMs;
        return true;
    }
    cv::Mat at(int64_t clipMs) override
    {
        const double want = double(clipMs) - startMs_;
        if (want < -frameMs_ / 2)
            return {};
        const exportpipe::VideoFrame* f = reader_.frameAt(std::llround(want + frameMs_ / 2));
        if (!f || std::fabs(double(f->ptsMs) - want) > frameMs_ / 2 + 1)
            return {};
        return cv::Mat(f->height, f->width, CV_8UC4, const_cast<uint8_t*>(f->rgba.data()));
    }
    bool refFrame() const override { return true; }
    double frameMs() const { return frameMs_; }
    double startMs() const { return startMs_; }
    void setStartMs(double ms) { startMs_ = ms; }

private:
    exportpipe::FlatVideoReader reader_;
    double frameMs_ = 33.37, startMs_ = 0;
};

// ref=template: our own stitch of the same frames from the file's data
// alone -- the new stitcher's base, without the capture-time correction or
// the seam colour blend -- whose geometry and colour hold still by
// construction: the reference for clips without a Studio export.
class TemplateReference : public Reference {
public:
    bool open(const Entry& e, const Options& opt, const camera::ClipMetadata& meta, std::string& err)
    {
        Entry t = e;
        t.method = Method::New;
        t.captureTiming = false;
        t.seamColourBlend = false;
        t.seam = insv::SeamCorrection{};
        t.colour = insv::LensColour{};
        std::string info;
        if (!src_.open(t, opt, meta, info)) {
            err = "template: " + src_.error;
            return false;
        }
        return true;
    }
    cv::Mat at(int64_t clipMs) override
    {
        while (!done_ && (!have_ || cur_.clipMs < clipMs)) {
            have_ = src_.next(cur_);
            done_ = !have_;
        }
        return have_ && cur_.clipMs == clipMs ? cur_.rgba : cv::Mat();
    }
    bool refFrame() const override { return false; }

private:
    CurrentStitcher src_;
    Picture cur_;
    bool have_ = false, done_ = false;
};

// --- output ------------------------------------------------------------------

std::string Num(double v)
{
    if (!std::isfinite(v))
        return "null";
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

std::string Str(const std::string& v)
{
    std::string o = "\"";
    for (char c : v) {
        if (c == '\n') {
            o += "\\n";
            continue;
        }
        if (c == '"' || c == '\\')
            o += '\\';
        o += c;
    }
    return o + "\"";
}

std::string Arr(const std::vector<double>& v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i)
        o += (i ? ", " : "") + Num(v[i]);
    return o + "]";
}

using Summary = std::vector<std::pair<std::string, double>>;

std::map<std::string, double> ReadSummary(const std::string& path)
{
    std::map<std::string, double> out;
    std::ifstream in(path);
    std::string line;
    bool inside = false;
    static const std::regex kv("^\\s*\"(\\w+)\": (\\S+?),?$");
    while (std::getline(in, line)) {
        if (!inside) {
            inside = line.find("\"summary\": {") != std::string::npos;
            continue;
        }
        if (line.find('}') != std::string::npos)
            break;
        std::smatch m;
        if (std::regex_match(line, m, kv))
            out[m[1]] = m[2] == "null" ? NAN : std::atof(m[2].str().c_str());
    }
    return out;
}

// A 4:4:4 YUV4MPEG2 stream: lossless enough to look at, plays in mpv /
// ffplay, needs no encoder. BT.601 limited range, what players assume.
class Y4mWriter {
public:
    bool open(const std::string& path, int w, int h, double fps)
    {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_)
            return false;
        w_ = w;
        h_ = h;
        std::fprintf(f_, "YUV4MPEG2 W%d H%d F%d:1000 Ip A1:1 C444\n", w, h, int(std::lround(fps * 1000)));
        return true;
    }
    void write(const cv::Mat& rgb) // CV_8UC3, w x h
    {
        std::vector<uint8_t> plane(size_t(w_) * h_ * 3);
        uint8_t *Y = plane.data(), *U = Y + size_t(w_) * h_, *V = U + size_t(w_) * h_;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                const cv::Vec3b p = rgb.at<cv::Vec3b>(y, x);
                const double r = p[0], g = p[1], b = p[2];
                const size_t i = size_t(y) * w_ + x;
                Y[i] = cv::saturate_cast<uint8_t>(16 + 0.2568 * r + 0.5041 * g + 0.0979 * b);
                U[i] = cv::saturate_cast<uint8_t>(128 - 0.1482 * r - 0.2910 * g + 0.4392 * b);
                V[i] = cv::saturate_cast<uint8_t>(128 + 0.4392 * r - 0.3678 * g - 0.0714 * b);
            }
        std::fputs("FRAME\n", f_);
        std::fwrite(plane.data(), 1, plane.size(), f_);
    }
    ~Y4mWriter()
    {
        if (f_)
            std::fclose(f_);
    }

private:
    std::FILE* f_ = nullptr;
    int w_ = 0, h_ = 0;
};

// The rows of a strip within +-halfDeg of the seam, resized to pxDeg, RGB.
cv::Mat VideoRow(const cv::Mat& stripRgba, const bench::StripGrid& g, double halfDeg, double pxDeg)
{
    const int r0 = int(std::lround((g.halfDeg - halfDeg) * g.pxDeg));
    const int r1 = int(std::lround((g.halfDeg + halfDeg) * g.pxDeg));
    cv::Mat rgb, out;
    cv::cvtColor(stripRgba.rowRange(r0, r1), rgb, cv::COLOR_RGBA2RGB);
    cv::resize(rgb, out, cv::Size(int(std::lround(360 * pxDeg)), int(std::lround(2 * halfDeg * pxDeg))), 0, 0,
               cv::INTER_AREA);
    return out;
}

// --- one stretch -----------------------------------------------------------

struct Series {
    std::vector<double> t, psnr, bandPsnr, psnrGain, bandPsnrGain, gain[3];
    std::vector<double> geomBandMed, geomBandP95, geomNearMed, geomNearP95, geomBodyMed, geomBodyP95;
    std::vector<double> rot[3];               // the suggested refrot, z y x
    std::vector<double> radial[2][bench::GeometryResult::kBins], tangential[2][bench::GeometryResult::kBins];
    std::vector<double> sharpBand, sharpBeside, ownSharp;
    std::vector<double> alive, aliveP90, aliveCtl;
    std::vector<double> colourSeam, colourSeamP90, colourControl, colourStep10, colourStep10P90;
};

// Median over frames of the per-bin means of bins [b0, b1].
double BinsMedian(const std::vector<double> (&bins)[bench::GeometryResult::kBins], int b0, int b1)
{
    std::vector<double> per;
    const size_t n = bins[b0].size();
    for (size_t i = 0; i < n; ++i) {
        double s = 0;
        int k = 0;
        for (int b = b0; b <= b1; ++b)
            if (i < bins[b].size() && std::isfinite(bins[b][i])) {
                s += bins[b][i];
                ++k;
            }
        per.push_back(k ? s / k : NAN);
    }
    return bench::Median(per);
}

bool RunEntry(const Entry& e, const Options& opt, std::string& err)
{
    std::printf("== %s: %s %.3f-%.3f s, %s%s%s\n", e.name.c_str(), e.clip.c_str(), e.startMs / 1000.0,
                e.endMs / 1000.0, e.stitcher.c_str(), e.note.empty() ? "" : " -- ", e.note.c_str());
    std::fflush(stdout);
    const auto wall0 = std::chrono::steady_clock::now();

    bench::SeamFrame frame;
    std::shared_ptr<const camera::ClipMetadata> meta;
    if (!SeamFrameOf(e, frame, meta, err))
        return false;
    const double fps = meta->fps > 0 ? meta->fps : 30.0;

    std::unique_ptr<Stitcher> stitcher;
    std::string info;
    if (e.stitcher == "current") {
        auto c = std::make_unique<CurrentStitcher>();
        if (!c->open(e, opt, *meta, info)) {
            err = c->error;
            return false;
        }
        stitcher = std::move(c);
    } else {
        auto v = std::make_unique<VideoStitcher>();
        if (!v->open(e.stitcher.substr(6), e)) {
            err = v->error;
            return false;
        }
        stitcher = std::move(v);
    }
    if (!info.empty())
        std::printf("  %s\n", info.c_str());

    std::unique_ptr<Reference> ref;
    VideoReference* videoRef = nullptr;
    if (e.ref == "template") {
        auto t = std::make_unique<TemplateReference>();
        if (!t->open(e, opt, *meta, err))
            return false;
        ref = std::move(t);
    } else if (!e.ref.empty()) {
        auto v = std::make_unique<VideoReference>();
        if (!v->open(e, err))
            return false;
        videoRef = v.get();
        ref = std::move(v);
    }
    const bool hasRef = ref != nullptr;
    const bench::M3 toRef = bench::RefRotation(e.refRotDeg[0], e.refRotDeg[1], e.refRotDeg[2]);
    const bench::M3* refRot = hasRef && ref->refFrame() ? &toRef : nullptr;

    const bench::StripGrid grid{20, 30};
    const bench::StripGrid half = grid.scaled(0.5);
    const bench::StripGrid colourGrid{4, 38}; // the colour round the seam, out to its control
    bench::Maps oursStrip, refStrip, refEq, oursEq, oursColour, refColour;
    std::vector<std::optional<bench::SeamColourBlocks>> colourRing(10); // the last 10 frames'
    cv::Mat elev;
    const int mW = opt.width, mH = opt.width / 2;
    if (opt.every > 0)
        elev = bench::ElevationMap(frame, mW, mH);

    Y4mWriter video;
    const double vHalf = 12;
    const int vRowH = int(std::lround(2 * vHalf * opt.videoPxDeg));
    const int vW = int(std::lround(360 * opt.videoPxDeg));
    const int vRows = hasRef ? 3 : 1, vGap = 4;
    if (opt.video && !video.open(opt.outDir + "/" + e.name + "_strip.y4m", vW,
                                 vRows * vRowH + (vRows - 1) * vGap, fps)) {
        err = "can't write the strip video";
        return false;
    }

    Series s;
    std::optional<bench::SeamOffsets> prevOffsets;
    double gain[3] = {1, 1, 1};
    int n = 0, refMissing = 0;
    Picture pic;
    bool have = stitcher->next(pic);

    // --fit-time: the reference frame that best matches the first frame,
    // within +-6 frames of refstart (PSNR of luma at 960, lined up). Not for
    // a video stitcher, which is on the reference's timing by definition.
    double timeFitMs = NAN;
    if (have && videoRef && opt.fitTime && !pic.refFrame) {
        const int fw = 960, fh = 480;
        const bench::Maps om = bench::EquirectMaps(fw, fh, pic.rgba.cols, pic.rgba.rows,
                                                   pic.refFrame ? toRef : bench::M3{});
        const cv::Mat og = bench::Grey(bench::Remap(pic.rgba, om));
        VideoReference& ref = *videoRef;
        const double given = ref.startMs();
        double best = -1, bestStart = given;
        for (int k = -6; k <= 6; ++k) {
            ref.setStartMs(given + k * ref.frameMs());
            const cv::Mat rf = ref.at(pic.clipMs);
            if (rf.empty())
                continue;
            const cv::Mat rg =
                bench::Grey(bench::Remap(rf, bench::EquirectMaps(fw, fh, rf.cols, rf.rows, toRef)));
            const double p = cv::PSNR(og, rg);
            if (p > best) {
                best = p;
                bestStart = ref.startMs();
            }
        }
        ref.setStartMs(bestStart);
        timeFitMs = bestStart;
        std::printf("  refstart fit: %.3f s (%+.0f frames; luma PSNR %.2f dB)\n", bestStart / 1000.0,
                    (bestStart - given) / ref.frameMs(), best);
    }

    for (; have; have = stitcher->next(pic)) {
        if (oursStrip.x.empty())
            oursStrip = bench::StripMaps(frame, grid, pic.rgba.cols, pic.rgba.rows,
                                         pic.refFrame ? &toRef : nullptr);
        const cv::Mat oStrip = bench::Remap(pic.rgba, oursStrip);
        cv::Mat oGrey = bench::Grey(oStrip), oHalf;
        cv::resize(oGrey, oHalf, cv::Size(half.width(), half.height()), 0, 0, cv::INTER_AREA);

        const cv::Mat rImg = hasRef ? ref->at(pic.clipMs) : cv::Mat();
        const bool rf = !rImg.empty();
        if (hasRef && !rf)
            ++refMissing;
        cv::Mat rStrip, rGrey, rHalf;
        if (rf) {
            if (refStrip.x.empty())
                refStrip = bench::StripMaps(frame, grid, rImg.cols, rImg.rows, refRot);
            rStrip = bench::Remap(rImg, refStrip);
            rGrey = bench::Grey(rStrip);
            cv::resize(rGrey, rHalf, cv::Size(half.width(), half.height()), 0, 0, cv::INTER_AREA);
        }

        s.t.push_back(double(pic.clipMs));
        // The colour round the seam against the reference's, and its change
        // over 10 frames (a missing reference frame breaks the chain).
        std::optional<bench::SeamColourBlocks>& colour10 = colourRing[n % colourRing.size()];
        if (rf) {
            if (oursColour.x.empty()) {
                oursColour = bench::StripMaps(frame, colourGrid, pic.rgba.cols, pic.rgba.rows,
                                              pic.refFrame ? &toRef : nullptr);
                refColour = bench::StripMaps(frame, colourGrid, rImg.cols, rImg.rows, refRot);
            }
            bench::SeamColourBlocks cb = bench::MeasureSeamColourBlocks(
                bench::Remap(rImg, refColour), bench::Remap(pic.rgba, oursColour), colourGrid);
            const bench::SeamColourResult d = bench::SeamColourDifference(cb);
            s.colourSeam.push_back(d.med);
            s.colourSeamP90.push_back(d.p90);
            s.colourControl.push_back(d.controlMed);
            if (colour10) {
                const bench::SeamColourResult c = bench::SeamColourChange(*colour10, cb);
                s.colourStep10.push_back(c.med);
                s.colourStep10P90.push_back(c.p90);
            }
            colour10 = std::move(cb);
        } else {
            for (auto& c : colourRing)
                c.reset();
        }
        // Alive: the offset from the reference along the seam against the
        // frame before's (a missing reference frame breaks the chain).
        if (rf) {
            bench::SeamOffsets off = bench::MeasureSeamOffsets(rHalf, oHalf, half, frame.bandDeg);
            if (prevOffsets) {
                const bench::AliveResult a = bench::CompareSeamOffsets(*prevOffsets, off);
                s.alive.push_back(a.bandMed);
                s.aliveP90.push_back(a.bandP90);
                s.aliveCtl.push_back(a.controlMed);
            }
            prevOffsets = std::move(off);
        } else {
            prevOffsets.reset();
        }

        const bench::GradientEnergy ge = bench::MeasureGradientEnergy(oGrey, grid, frame.bandDeg);
        s.ownSharp.push_back(ge.band / ge.beside);
        if (rf) {
            const bench::GradientEnergy rge = bench::MeasureGradientEnergy(rGrey, grid, frame.bandDeg);
            s.sharpBand.push_back(ge.band / rge.band);
            s.sharpBeside.push_back(ge.beside / rge.beside);
        }

        // The whole sphere, every N-th frame.
        const bool whole = rf && opt.every > 0 && n % opt.every == 0;
        s.psnr.push_back(NAN);
        s.bandPsnr.push_back(NAN);
        if (whole) {
            if (refRot && refEq.x.empty())
                refEq = bench::EquirectMaps(rImg.cols, rImg.rows, rImg.cols, rImg.rows, toRef);
            cv::Mat oEq, rEq;
            if (pic.refFrame) {
                if (oursEq.x.empty())
                    oursEq = bench::EquirectMaps(pic.rgba.cols, pic.rgba.rows, pic.rgba.cols,
                                                 pic.rgba.rows, toRef);
                cv::resize(bench::Remap(pic.rgba, oursEq), oEq, cv::Size(mW, mH), 0, 0, cv::INTER_AREA);
            } else {
                cv::resize(pic.rgba, oEq, cv::Size(mW, mH), 0, 0, cv::INTER_AREA);
            }
            cv::resize(refRot ? bench::Remap(rImg, refEq) : rImg, rEq, cv::Size(mW, mH), 0, 0,
                       cv::INTER_AREA);
            const bench::PsnrResult p = bench::ComparePsnr(oEq, rEq, elev, frame.bandDeg);
            s.psnr.back() = p.whole;
            s.bandPsnr.back() = p.band;
            s.psnrGain.push_back(p.wholeGain);
            s.bandPsnrGain.push_back(p.bandGain);
            for (int c = 0; c < 3; ++c) {
                s.gain[c].push_back(p.gain[c]);
                gain[c] = p.gain[c];
            }
            const bench::GeometryResult g = bench::MeasureGeometry(oEq, rEq, frame);
            s.geomBandMed.push_back(g.bandMed);
            s.geomBandP95.push_back(g.bandP95);
            s.geomNearMed.push_back(g.nearMed);
            s.geomNearP95.push_back(g.nearP95);
            s.geomBodyMed.push_back(g.bodyMed);
            s.geomBodyP95.push_back(g.bodyP95);
            double fixed[3];
            bench::RotFitRefrot(g.rotation, e.refRotDeg, fixed);
            for (int k = 0; k < 3; ++k)
                s.rot[k].push_back(fixed[k]);
            for (int l = 0; l < 2; ++l)
                for (int b = 0; b < bench::GeometryResult::kBins; ++b) {
                    s.radial[l][b].push_back(g.radial[l][b]);
                    s.tangential[l][b].push_back(g.tangential[l][b]);
                }
        }

        if (opt.video) {
            cv::Mat out = cv::Mat::zeros(vRows * vRowH + (vRows - 1) * vGap, vW, CV_8UC3);
            const cv::Mat o = VideoRow(oStrip, grid, vHalf, opt.videoPxDeg);
            o.copyTo(out.rowRange(0, vRowH));
            if (vRows == 3) {
                if (!rStrip.empty()) {
                    const cv::Mat r = VideoRow(rStrip, grid, vHalf, opt.videoPxDeg);
                    r.copyTo(out.rowRange(vRowH + vGap, 2 * vRowH + vGap));
                    cv::Mat of, rfl, diff;
                    o.convertTo(of, CV_32FC3);
                    of = of.mul(cv::Scalar(gain[0], gain[1], gain[2]));
                    r.convertTo(rfl, CV_32FC3);
                    cv::Mat(cv::abs(of - rfl) * 4).convertTo(diff, CV_8UC3);
                    diff.copyTo(out.rowRange(2 * (vRowH + vGap), 3 * vRowH + 2 * vGap));
                }
            }
            video.write(out);
        }
        ++n;
        if (n % 30 == 0) {
            std::printf("  %d frames, %.0f s\n", n,
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count());
            std::fflush(stdout);
        }
    }
    if (!stitcher->error.empty()) {
        err = stitcher->error;
        return false;
    }
    if (n == 0) {
        err = "no frames in the stretch";
        return false;
    }
    const double wallSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();

    using bench::Median;
    using bench::Percentile;
    Summary sum;
    sum.emplace_back("frames", n);
    if (hasRef) {
        sum.emplace_back("ref_missing", refMissing);
        if (std::isfinite(timeFitMs))
            sum.emplace_back("refstart_fit_s", timeFitMs / 1000.0);
        sum.emplace_back("psnr", Median(s.psnr));
        sum.emplace_back("band_psnr", Median(s.bandPsnr));
        sum.emplace_back("psnr_gain", Median(s.psnrGain));
        sum.emplace_back("band_psnr_gain", Median(s.bandPsnrGain));
        sum.emplace_back("gain_r_pct", (Median(s.gain[0]) - 1) * 100);
        sum.emplace_back("gain_g_pct", (Median(s.gain[1]) - 1) * 100);
        sum.emplace_back("gain_b_pct", (Median(s.gain[2]) - 1) * 100);
        sum.emplace_back("geom_band_med", Median(s.geomBandMed));
        sum.emplace_back("geom_band_p95", Median(s.geomBandP95));
        sum.emplace_back("geom_near_med", Median(s.geomNearMed));
        sum.emplace_back("geom_near_p95", Median(s.geomNearP95));
        sum.emplace_back("geom_body_med", Median(s.geomBodyMed));
        sum.emplace_back("geom_body_p95", Median(s.geomBodyP95));
        sum.emplace_back("refrot_fit_z", Median(s.rot[0]));
        sum.emplace_back("refrot_fit_y", Median(s.rot[1]));
        sum.emplace_back("refrot_fit_x", Median(s.rot[2]));
        sum.emplace_back("lens0_radial_50_80", BinsMedian(s.radial[0], 5, 7));
        sum.emplace_back("lens1_radial_50_80", BinsMedian(s.radial[1], 5, 7));
        sum.emplace_back("lens0_tangential_50_80", BinsMedian(s.tangential[0], 5, 7));
        sum.emplace_back("lens1_tangential_50_80", BinsMedian(s.tangential[1], 5, 7));
        sum.emplace_back("sharp_band", Median(s.sharpBand));
        sum.emplace_back("sharp_beside", Median(s.sharpBeside));
        sum.emplace_back("colour_seam", Median(s.colourSeam));
        sum.emplace_back("colour_seam_p90", Median(s.colourSeamP90));
        sum.emplace_back("colour_control", Median(s.colourControl));
        sum.emplace_back("colour_step10", Median(s.colourStep10));
        sum.emplace_back("colour_step10_p90", Median(s.colourStep10P90));
    }
    sum.emplace_back("own_sharp_band_beside", Median(s.ownSharp));
    if (hasRef) {
        // Means over frames: the moments the seam jumps are what is seen.
        auto mean = [](const std::vector<double>& v) {
            double t = 0;
            int k = 0;
            for (double x : v)
                if (std::isfinite(x)) {
                    t += x;
                    ++k;
                }
            return k ? t / k : NAN;
        };
        sum.emplace_back("alive_band", mean(s.alive));
        sum.emplace_back("alive_band_p90", mean(s.aliveP90));
        sum.emplace_back("alive_control", mean(s.aliveCtl));
        // The frames where the seam jumps: the 99th percentile over frames.
        sum.emplace_back("alive_peak", Percentile(s.aliveP90, 0.99));
    }

    // The JSON: the summary one "key": number per line, so --baseline reads
    // it back without a JSON library.
    std::ostringstream j;
    j << "{\n  \"name\": " << Str(e.name) << ",\n  \"clip\": " << Str(e.clip)
      << ",\n  \"start_ms\": " << e.startMs << ",\n  \"end_ms\": " << e.endMs
      << ",\n  \"stitcher\": " << Str(e.stitcher) << ",\n  \"guard\": " << Str(GuardName(e.guard))
      << ",\n  \"method\": " << Str(MethodName(e.method)) << ",\n  \"note\": " << Str(e.note)
      << ",\n  \"info\": " << Str(info) << ",\n  \"ref\": " << Str(e.ref)
      << ",\n  \"band_deg\": " << Num(frame.bandDeg) << ",\n  \"wall_s\": " << Num(wallSec)
      << ",\n  \"summary\": {\n";
    for (size_t i = 0; i < sum.size(); ++i)
        j << "    \"" << sum[i].first << "\": " << Num(sum[i].second) << (i + 1 < sum.size() ? ",\n" : "\n");
    j << "  }";
    auto series = [&](const char* k, const std::vector<double>& v) {
        j << ",\n  \"" << k << "\": " << Arr(v);
    };
    series("t_ms", s.t);
    series("psnr", s.psnr);
    series("band_psnr", s.bandPsnr);
    series("alive_band", s.alive);
    series("alive_band_p90", s.aliveP90);
    series("colour_seam", s.colourSeam);
    series("colour_step10", s.colourStep10);
    for (int l = 0; l < 2; ++l) {
        std::vector<double> rad, tan;
        for (int b = 0; b < bench::GeometryResult::kBins; ++b) {
            rad.push_back(Median(s.radial[l][b]));
            tan.push_back(Median(s.tangential[l][b]));
        }
        series(l == 0 ? "lens0_radial_per_10deg" : "lens1_radial_per_10deg", rad);
        series(l == 0 ? "lens0_tangential_per_10deg" : "lens1_tangential_per_10deg", tan);
    }
    j << "\n}\n";
    std::ofstream(opt.outDir + "/" + e.name + ".json") << j.str();

    std::map<std::string, double> base;
    if (!opt.baseline.empty()) {
        base = ReadSummary(opt.baseline + "/" + e.name + ".json");
        if (base.empty())
            std::printf("  (no baseline for %s)\n", e.name.c_str());
    }
    for (const auto& [k, v] : sum) {
        const auto it = base.find(k);
        const std::string now = Num(v);
        if (it == base.end() || Num(it->second) == now)
            std::printf("  %-24s %s\n", k.c_str(), now.c_str());
        else
            std::printf("  %-24s %s  (was %s)  *\n", k.c_str(), now.c_str(), Num(it->second).c_str());
    }
    std::printf("  %d frames in %.0f s\n", n, wallSec);
    std::fflush(stdout);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    std::string list;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (a == "--out")
            opt.outDir = next();
        else if (a == "--baseline")
            opt.baseline = next();
        else if (a == "--only")
            opt.only = next();
        else if (a == "--width")
            opt.width = std::max(360, std::atoi(next().c_str()) / 2 * 2);
        else if (a == "--every")
            opt.every = std::atoi(next().c_str());
        else if (a == "--video-px-deg")
            opt.videoPxDeg = std::clamp(std::atof(next().c_str()), 1.0, 20.0);
        else if (a == "--no-video")
            opt.video = false;
        else if (a == "--live")
            opt.live = true;
        else if (a == "--fit-time")
            opt.fitTime = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n%s", a.c_str(), kUsage);
            return 2;
        } else
            list = a;
    }
    if (list.empty() || opt.outDir.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    std::vector<Entry> entries;
    std::string err;
    if (!ParseList(list, entries, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(opt.outDir, ec);
    if (ec) {
        std::fprintf(stderr, "can't create %s: %s\n", opt.outDir.c_str(), ec.message().c_str());
        return 1;
    }
    int failed = 0, ran = 0;
    for (const Entry& e : entries) {
        if (!opt.only.empty() && e.name.find(opt.only) == std::string::npos)
            continue;
        ++ran;
        std::string why;
        if (!RunEntry(e, opt, why)) {
            std::printf("  FAILED: %s\n", why.c_str());
            ++failed;
        }
    }
    std::printf("%d stretch(es), %d failed\n", ran, failed);
    return failed ? 1 : 0;
}
