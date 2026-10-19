#include "PlateScanJob.h"

#include <PlateScanner.h>
#include <StitchFrameSource.h>

#include <GyroStabilizer.h>
#include <ClipMetadata.h>
#include <NativeStitchSource.h>

#include <chrono>
#include <cstring>
#include <memory>

namespace platescan {

namespace {

// The stitch as playback / export set it up, seeked to req.fromMs, and the
// clip's Horizon lock (levels the detector's views; its gyro also drives the
// capture-time correction when req.captureTiming, as
// ReframeExportController::captureTimingFor does for playback / export with
// the stabilization on). *timing: the stitch it set up.
bool openStitch(const Request& req, stitch::NativeStitchSource& src,
                std::shared_ptr<insv::GyroStabilizer>& level, privacy::StitchTiming* timing, std::string& err)
{
    level = std::make_shared<insv::GyroStabilizer>();
    if (const auto imu = camera::ReadClipImu(req.clipPath))
        level->build(*imu, insv::StabConfig::preset(insv::StabMode::HorizonLock),
                     req.syncAdjustMs / 1000.0);
    // The views come straight from the lenses; 7680 is the photometric
    // measure grid, and the equirect the CPU fallback cuts them from.
    stitch::PreviewStitchConfig sc;
    sc.width = 7680;
    sc.height = 3840;
    sc.lensGuard = req.lensGuard;
    sc.depthSeam = req.depthSeam;
    if (!src.Open(req.clipPath, sc) || !src.SeekTo(req.fromMs)) {
        err = src.GetLastError();
        return false;
    }
    src.SetLens1Adjust(req.seamAdjustDeg.data());
    *timing = !level->ready()       ? privacy::StitchTiming::NoGyro
              : req.captureTiming ? privacy::StitchTiming::Corrected
                                  : privacy::StitchTiming::Uncorrected;
    if (*timing == privacy::StitchTiming::Corrected) {
        stitch::CaptureTiming t;
        t.readoutSec[0] = t.readoutSec[1] = level->readoutSec();
        t.rotationAt = [level](double tSec, double dtSec, float r[3]) { level->bodyRotation(tSec, dtSec, r); };
        src.SetCaptureTiming(std::move(t));
    }
    return true;
}

} // namespace

Result run(const Request& req, const std::atomic<bool>& stop, const ProgressFn& progress)
{
    using Clock = std::chrono::steady_clock;
    Result res;
    const double fps = req.fps;

    privacy::PlateScanner scanner;
    if (!scanner.init(privacy::ScanParams{}, res.error))
        return res;
    res.device = scanner.device();
    stitch::NativeStitchSource src;
    std::shared_ptr<insv::GyroStabilizer> level;
    privacy::StitchTiming timing = privacy::StitchTiming::Corrected;
    if (!openStitch(req, src, level, &timing, res.error))
        return res;

    privacy::StitchFrameSource frames(src, req.fromMs, req.toMs, fps, level);
    const auto t0 = Clock::now();
    int n = 0;
    std::string err;
    const bool ok = scanner.run(frames, &stop,
                                [&](int64_t tMs) {
                                    ++n;
                                    const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
                                    res.fpsAchieved = sec > 0.0 ? n / sec : 0.0;
                                    if (progress && n % 5 == 0)
                                        progress(tMs, res.fpsAchieved);
                                },
                                err);
    res.stopped = stop.load();
    if (!ok && n == 0)
        res.error = err;
    if (res.error.empty()) {
        res.scan = scanner.finish(fps);
        for (privacy::PlateScan::Span& sp : res.scan.spans)
            sp.timing = timing;
    }
    return res;
}

} // namespace platescan

namespace platescan {

FollowResult follow(const Request& req, const insv::BlurQuad& seed, int64_t backToMs,
                    const std::atomic<bool>& stop, const FollowProgressFn& progress)
{
    FollowResult res;
    const double fps = req.fps;
    privacy::PlateScanner scanner;
    if (!scanner.init(privacy::ScanParams{}, res.error))
        return res;
    stitch::NativeStitchSource src;
    std::shared_ptr<insv::GyroStabilizer> level;
    if (!openStitch(req, src, level, &res.timing, res.error))
        return res;
    int n = 0;
    auto step = [&](bool backward) {
        return [&, backward](int64_t tMs, bool) {
            if (progress && ++n % 3 == 0)
                progress(tMs, backward);
        };
    };
    {
        privacy::StitchFrameSource frames(src, req.fromMs, req.toMs, fps, level);
        if (!scanner.follow(frames, seed, privacy::FollowParams{}, &stop, step(false), res.follow, res.error))
            return res;
    }
    res.error.clear();
    if (backToMs >= 0 && backToMs < req.fromMs && !stop.load() && !res.follow.frames.empty()) {
        // Back from the same frame and box; its first frame is the forward
        // pass's first, so it is dropped.
        privacy::ReverseStitchFrameSource frames(src, backToMs, req.fromMs, fps, level);
        privacy::FollowResult back;
        std::string err;
        if (scanner.follow(frames, seed, privacy::FollowParams{}, &stop, step(true), back, err) &&
            back.frames.size() > 1) {
            std::vector<privacy::ScanFrame> all(back.frames.rbegin(), back.frames.rend() - 1);
            all.insert(all.end(), std::make_move_iterator(res.follow.frames.begin()),
                       std::make_move_iterator(res.follow.frames.end()));
            res.follow.frames = std::move(all);
            res.follow.detections += back.detections;
            res.lostBackward = back.lost;
        }
        if (!err.empty())
            res.error = "following backwards: " + err;
    }
    res.stopped = stop.load();
    return res;
}

privacy::BlurAddition toBlurAddition(const FollowResult& r)
{
    privacy::BlurAddition add;
    add.timing = r.timing;
    for (const privacy::ScanFrame& f : r.follow.frames)
        for (const privacy::BlurRegion& reg : f.regions) {
            privacy::BlurAddition::Frame af;
            af.tMs = f.tMs;
            std::memcpy(af.corner, reg.quad.corner, sizeof(af.corner));
            af.kind = static_cast<uint8_t>(reg.kind);
            add.frames.push_back(af);
        }
    return add;
}

} // namespace platescan
