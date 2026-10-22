#include "SubjectTracker.h"

#include "DataPaths.h"

#include <GyroStabilizer.h>  // insv::leanWindowFromStrength
#include <ReframeRenderer.h> // exportpipe::RenderRectilinear

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/tracking.hpp>       // TrackerCSRT (contrib)
#include <opencv2/video/tracking.hpp> // TrackerVit, TrackerNano

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <numbers>

namespace track {

namespace {

constexpr double kPi = std::numbers::pi;

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string vitPath(const std::string& dir) { return dir + "/vittrack.onnx"; }
std::string nanoBackbonePath(const std::string& dir) { return dir + "/nanotrack_backbone_sim.onnx"; }
std::string nanoHeadPath(const std::string& dir) { return dir + "/nanotrack_head_sim.onnx"; }

std::string modelDirOf(const TrackerParams& p)
{
    return p.modelDir.empty() ? defaultModelDir() : p.modelDir;
}

// A square rectilinear view of the stabilized sphere.
struct Crop {
    double yaw = 0.0, pitch = 0.0, fov = 1.0; // centre (stabilized) + FOV, radians
    int size = 512;

    double tanHalf() const { return std::tan(fov * 0.5); }
    Vec3 pixelToDir(double px, double py) const
    {
        return viewPointToStab(px / size, py / size, yaw, pitch, 0.0, fov, 0.0, 1.0);
    }
    double pxToAngle(double px) const { return 2.0 * std::atan(px / size * tanHalf()); }
    double angleToPx(double ang) const { return std::tan(ang * 0.5) / tanHalf() * size; }
};

cv::Mat renderCrop(const uint8_t* eq, int w, int h, const Crop& c, const float stab[9])
{
    exportpipe::ReframeView v;
    v.pose.yaw = static_cast<float>(c.yaw);
    v.pose.pitch = static_cast<float>(c.pitch);
    v.pose.fov = view::fovFor<double>(static_cast<float>(c.fov), 0.0, 1.0); // rectilinear, square
    v.pose.distortion = 0.0f;
    v.aspect = 1.0f;
    std::copy(stab, stab + 9, v.stab);
    cv::Mat rgba(c.size, c.size, CV_8UC4);
    exportpipe::RenderRectilinear(eq, w, h, rgba.data, c.size, c.size, v);
    cv::Mat bgr;
    cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
    return bgr;
}

// One OpenCV tracker behind a common init / update(score).
struct Tracker {
    TrackerKind kind = TrackerKind::Csrt;
    std::string modelDir;
    cv::Ptr<cv::Tracker> t;

    void create()
    {
        if (kind == TrackerKind::Vit) {
            cv::TrackerVit::Params p;
            p.net = vitPath(modelDir);
            t = cv::TrackerVit::create(p);
        } else if (kind == TrackerKind::Nano) {
            cv::TrackerNano::Params p;
            p.backbone = nanoBackbonePath(modelDir);
            p.neckhead = nanoHeadPath(modelDir);
            t = cv::TrackerNano::create(p);
        } else {
            t = cv::TrackerCSRT::create();
        }
    }
    void init(const cv::Mat& img, const cv::Rect& box)
    {
        // CSRT gets a fresh object (its init() doesn't reset all state); the
        // ONNX trackers just re-init, so the net isn't reloaded every time.
        if (!t || kind == TrackerKind::Csrt)
            create();
        t->init(img, box);
    }
    // Score in [0,1]; CSRT has none -> 1 on success, 0 on failure.
    double update(const cv::Mat& img, cv::Rect& box)
    {
        const bool ok = t->update(img, box);
        if (kind == TrackerKind::Vit)
            return static_cast<cv::TrackerVit*>(t.get())->getTrackingScore();
        if (kind == TrackerKind::Nano)
            return static_cast<cv::TrackerNano*>(t.get())->getTrackingScore();
        return ok ? 1.0 : 0.0;
    }
};

} // namespace

std::string defaultModelDir()
{
    if (const char* e = std::getenv("ORBIS_TRACK_MODEL_DIR"))
        return e;
    return core::paths::modelDir("vittrack");
}

bool modelsAvailable(const TrackerParams& params)
{
    namespace fs = std::filesystem;
    const std::string dir = modelDirOf(params);
    std::error_code ec;
    switch (params.kind) {
    case TrackerKind::Csrt:
        return true;
    case TrackerKind::Nano:
        return fs::exists(nanoBackbonePath(dir), ec) && fs::exists(nanoHeadPath(dir), ec);
    case TrackerKind::Vit:
    case TrackerKind::Hybrid:
        return fs::exists(vitPath(dir), ec);
    }
    return false;
}

struct SubjectTracker::Impl {
    TrackerParams p;
    Tracker tracker;  // Hybrid: CSRT
    Tracker verifier; // Hybrid: VitTrack
    Crop crop;
    double tgtYaw = 0.0, tgtPitch = 0.0, tgtW = 0.0, tgtH = 0.0;
    struct VerifyEntry {
        cv::Mat img;
        cv::Rect box;
    };
    std::deque<VerifyEntry> recentGood; // verified crops, oldest first
    bool keepDebug = false;
    std::vector<uint8_t> debugCrop;
    int debugBox[4] = {0, 0, 0, 0};
    double debugFov = 0.0;
    Timing timing;

    bool hybrid() const { return p.kind == TrackerKind::Hybrid; }
    double fovFor(double sizeRad) const
    {
        return std::clamp(sizeRad * p.fovFactor, p.minFovRad, p.maxFovRad);
    }
    cv::Rect centredBox(const Crop& c) const
    {
        const double w = c.angleToPx(tgtW), h = c.angleToPx(tgtH);
        return cv::Rect(cvRound(c.size * 0.5 - w * 0.5), cvRound(c.size * 0.5 - h * 0.5),
                        std::max(4, cvRound(w)), std::max(4, cvRound(h)));
    }
    Crop verifyCropAt() const
    {
        Crop c;
        c.size = p.verifyCrop;
        c.yaw = tgtYaw;
        c.pitch = tgtPitch;
        c.fov = fovFor(std::max(tgtW, tgtH));
        return c;
    }
    void keep(const cv::Mat& img, const cv::Rect& box)
    {
        if (!keepDebug)
            return;
        debugCrop.assign(img.data, img.data + img.total() * img.elemSize());
        debugBox[0] = box.x;
        debugBox[1] = box.y;
        debugBox[2] = box.width;
        debugBox[3] = box.height;
        debugFov = crop.fov;
    }
};

SubjectTracker::SubjectTracker(const TrackerParams& params) : d(std::make_unique<Impl>())
{
    d->p = params;
    const std::string dir = modelDirOf(params);
    d->tracker.kind = params.kind == TrackerKind::Hybrid ? TrackerKind::Csrt : params.kind;
    d->tracker.modelDir = dir;
    d->verifier.kind = TrackerKind::Vit;
    d->verifier.modelDir = dir;
}

SubjectTracker::~SubjectTracker() = default;

void SubjectTracker::setKeepDebug(bool on) { d->keepDebug = on; }
const std::vector<uint8_t>& SubjectTracker::debugCrop() const { return d->debugCrop; }
const int* SubjectTracker::debugBox() const { return d->debugBox; }
double SubjectTracker::debugFovRad() const { return d->debugFov; }
const SubjectTracker::Timing& SubjectTracker::timing() const { return d->timing; }

bool SubjectTracker::start(const uint8_t* eqRgba, int w, int h, const float stab[9],
                           const Seed& seed, int64_t /*tMs*/, std::string& err)
{
    if (!modelsAvailable(d->p)) {
        err = "tracking model not found in " + modelDirOf(d->p);
        return false;
    }
    d->tgtYaw = seed.yaw;
    d->tgtPitch = seed.pitch;
    d->tgtW = std::max(seed.width, 1e-3);
    d->tgtH = std::max(seed.height, 1e-3);
    d->crop = Crop{};
    d->crop.size = d->p.crop;
    d->crop.yaw = d->tgtYaw;
    d->crop.pitch = d->tgtPitch;
    d->crop.fov = d->fovFor(std::max(d->tgtW, d->tgtH));
    d->recentGood.clear();
    d->timing = Timing{};
    try {
        const cv::Mat img = renderCrop(eqRgba, w, h, d->crop, stab);
        const cv::Rect box = d->centredBox(d->crop);
        d->tracker.init(img, box);
        d->keep(img, box);
        if (d->hybrid()) {
            const Crop vc = d->verifyCropAt();
            d->recentGood.push_back({renderCrop(eqRgba, w, h, vc, stab), d->centredBox(vc)});
            d->verifier.create(); // loads the net now, so a bad file fails here
        }
    } catch (const cv::Exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

TrackSample SubjectTracker::step(const uint8_t* eqRgba, int w, int h, const float stab[9],
                                 int64_t tMs)
{
    Impl& s = *d;
    TrackSample out;
    out.tMs = tMs;

    auto c0 = Clock::now();
    const cv::Mat img = renderCrop(eqRgba, w, h, s.crop, stab);
    s.timing.cropMs += msSince(c0);

    cv::Rect box;
    auto t0 = Clock::now();
    double score = s.tracker.update(img, box);
    s.timing.trackerMs += msSince(t0);
    s.keep(img, box);

    const double cx = box.x + box.width * 0.5, cy = box.y + box.height * 0.5;
    if (s.hybrid()) {
        // CSRT always moves the target; VitTrack decides whether to trust it.
        yawPitchFromDir(s.crop.pixelToDir(cx, cy), s.tgtYaw, s.tgtPitch);
        s.tgtW = s.crop.pxToAngle(box.width);
        s.tgtH = s.crop.pxToAngle(box.height);
        auto v0 = Clock::now();
        const Crop vc = s.verifyCropAt();
        const cv::Mat cur = renderCrop(eqRgba, w, h, vc, stab);
        // Reference: the verified crop refLag good frames back -- or, while
        // lost, the last good appearance.
        const Impl::VerifyEntry& ref = s.recentGood.front();
        s.verifier.init(ref.img, ref.box);
        cv::Rect vbox;
        const double vs = s.verifier.update(cur, vbox);
        const double off = std::hypot(vbox.x + vbox.width * 0.5 - vc.size * 0.5,
                                      vbox.y + vbox.height * 0.5 - vc.size * 0.5) / vc.size;
        score = off <= s.p.verifyOffFrac ? vs : 0.0;
        if (score >= s.p.lostScore) {
            s.recentGood.push_back({cur, s.centredBox(vc)});
            while (static_cast<int>(s.recentGood.size()) > std::max(1, s.p.refLag))
                s.recentGood.pop_front();
        }
        s.timing.verifyMs += msSince(v0);
    }
    out.score = score;
    out.good = score >= s.p.lostScore;
    if (out.good && !s.hybrid()) {
        yawPitchFromDir(s.crop.pixelToDir(cx, cy), s.tgtYaw, s.tgtPitch);
        s.tgtW = s.crop.pxToAngle(box.width);
        s.tgtH = s.crop.pxToAngle(box.height);
    }

    // Re-centre (same frame, re-rendered) when the subject has drifted
    // off-centre or its size no longer suits the crop FOV. Hybrid follows CSRT
    // even while unverified: the verifier may find it again.
    if (out.good || s.hybrid()) {
        const double offC = std::hypot(cx - s.crop.size * 0.5, cy - s.crop.size * 0.5) / s.crop.size;
        const double want = s.fovFor(std::max(s.tgtW, s.tgtH));
        const bool sizeOff = want < s.crop.fov * 0.7 || want > s.crop.fov * 1.4;
        if (offC > s.p.recenterFrac || sizeOff || s.p.recenterFrac <= 0.0) {
            s.crop.yaw = s.tgtYaw;
            s.crop.pitch = s.tgtPitch;
            s.crop.fov = want;
            auto r0 = Clock::now();
            const cv::Mat img2 = renderCrop(eqRgba, w, h, s.crop, stab);
            s.timing.cropMs += msSince(r0);
            s.tracker.init(img2, s.centredBox(s.crop));
            out.reinit = true;
            ++s.timing.reinits;
        }
    }

    out.yaw = s.tgtYaw;
    out.pitch = s.tgtPitch;
    out.size = std::max(s.tgtW, s.tgtH);
    out.width = s.tgtW;
    out.height = s.tgtH;
    return out;
}

// --- path -> keys ---------------------------------------------------------

namespace {

void fillGaps(std::vector<double>& v, const std::vector<bool>& good)
{
    const int n = static_cast<int>(v.size());
    int prev = -1;
    for (int i = 0; i < n; ++i) {
        if (!good[size_t(i)])
            continue;
        if (prev < 0) {
            for (int j = 0; j < i; ++j)
                v[size_t(j)] = v[size_t(i)];
        } else if (i - prev > 1) {
            for (int j = prev + 1; j < i; ++j)
                v[size_t(j)] = v[size_t(prev)] +
                               (v[size_t(i)] - v[size_t(prev)]) * (j - prev) / double(i - prev);
        }
        prev = i;
    }
    if (prev >= 0)
        for (int j = prev + 1; j < n; ++j)
            v[size_t(j)] = v[size_t(prev)];
}

std::vector<double> gaussSmooth(const std::vector<double>& v, double sigmaSamples)
{
    if (sigmaSamples < 0.5)
        return v;
    const int r = static_cast<int>(std::ceil(3.0 * sigmaSamples));
    const int n = static_cast<int>(v.size());
    std::vector<double> out(v.size());
    for (int i = 0; i < n; ++i) {
        double sum = 0.0, wsum = 0.0;
        for (int k = -r; k <= r; ++k) {
            const int j = std::clamp(i + k, 0, n - 1);
            const double g = std::exp(-0.5 * k * k / (sigmaSamples * sigmaSamples));
            sum += g * v[size_t(j)];
            wsum += g;
        }
        out[size_t(i)] = sum / wsum;
    }
    return out;
}

void rdp(const std::vector<PathKey>& p, int i0, int i1, double tol, std::vector<bool>& keep)
{
    if (i1 - i0 < 2)
        return;
    double worst = -1.0;
    int wi = -1;
    for (int i = i0 + 1; i < i1; ++i) {
        const double f = double(p[size_t(i)].tMs - p[size_t(i0)].tMs) /
                         double(std::max<int64_t>(1, p[size_t(i1)].tMs - p[size_t(i0)].tMs));
        const double y = p[size_t(i0)].yaw + (p[size_t(i1)].yaw - p[size_t(i0)].yaw) * f;
        const double q = p[size_t(i0)].pitch + (p[size_t(i1)].pitch - p[size_t(i0)].pitch) * f;
        const double e = angleBetween(dirFromYawPitch(p[size_t(i)].yaw, p[size_t(i)].pitch),
                                      dirFromYawPitch(y, q));
        if (e > worst) {
            worst = e;
            wi = i;
        }
    }
    if (worst > tol) {
        keep[size_t(wi)] = true;
        rdp(p, i0, wi, tol, keep);
        rdp(p, wi, i1, tol, keep);
    }
}

} // namespace

std::vector<PathKey> reduceToKeys(const std::vector<TrackSample>& samples, double fps,
                                  const ReduceParams& params, std::vector<PathKey>* smoothed)
{
    const size_t n = samples.size();
    if (n == 0)
        return {};
    std::vector<double> y(n), p(n);
    std::vector<bool> good(n);
    for (size_t i = 0; i < n; ++i) {
        y[i] = samples[i].yaw;
        p[i] = samples[i].pitch;
        good[i] = samples[i].good;
    }
    for (size_t i = 1; i < n; ++i) { // unwrap yaw
        while (y[i] - y[i - 1] > kPi)
            y[i] -= 2.0 * kPi;
        while (y[i] - y[i - 1] < -kPi)
            y[i] += 2.0 * kPi;
    }
    fillGaps(y, good);
    fillGaps(p, good);
    const double sigma =
        insv::leanWindowFromStrength(params.smoothStrength) * fps / 3.0;
    const std::vector<double> ys = gaussSmooth(y, sigma), ps = gaussSmooth(p, sigma);

    std::vector<PathKey> path(n);
    for (size_t i = 0; i < n; ++i)
        path[i] = {samples[i].tMs, ys[i], ps[i]};
    std::vector<bool> keep(n, false);
    keep.front() = keep.back() = true;
    rdp(path, 0, static_cast<int>(n) - 1, params.tolRad, keep);
    std::vector<PathKey> keys;
    for (size_t i = 0; i < n; ++i)
        if (keep[i])
            keys.push_back(path[i]);
    if (smoothed)
        *smoothed = std::move(path);
    return keys;
}

} // namespace track
