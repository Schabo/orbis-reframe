#include "SubjectTrackJob.h"

#include <GyroStabilizer.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace subjecttrack {

Result run(const Request& req, const std::atomic<bool>& stop, const FrameFn& onFrame)
{
    using Clock = std::chrono::steady_clock;
    Result res;
    stitch::PreviewStitchConfig sc;
    sc.width = req.workWidth & ~1;
    sc.height = (sc.width / 2) & ~1;
    sc.lensGuard = req.lensGuard;
    stitch::NativeStitchSource src;
    if (!src.Open(req.clipPath, sc) || !src.SeekTo(req.fromMs)) {
        res.error = src.GetLastError();
        return res;
    }
    src.SetSeamCorrection(req.seamCorrection);
    if (req.blur)
        src.SetBlurQuads(req.blur);

    track::SubjectTracker tracker;
    const double halfFrameMs = 500.0 / req.fps;
    const int maxLost =
        req.maxLostSec > 0.0 ? std::max(1, static_cast<int>(req.maxLostSec * req.fps)) : 0;
    int lostRun = 0;
    size_t lastGood = 0;
    bool started = false;
    Clock::time_point t0{};
    int64_t firstMs = 0;
    int frames = 0;
    double idleSec = 0.0; // pacing sleeps, left out of fpsAchieved
    for (;;) {
        if (stop.load()) {
            res.stopped = true;
            break;
        }
        stitch::Frame f = src.ReadFrame();
        if (f.rgba.empty()) {
            if (!src.GetLastError().empty() && !started)
                res.error = src.GetLastError();
            break;
        }
        if (f.timestamp_ms + halfFrameMs < req.fromMs)
            continue;
        if (f.timestamp_ms > req.toMs)
            break;
        float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        if (req.stab)
            req.stab->correctionMatrix(f.timestamp_ms / 1000.0, m);
        track::TrackSample s;
        if (!started) {
            if (!tracker.start(f.rgba.data(), f.width, f.height, &m[0][0], req.seed, f.timestamp_ms,
                               res.error))
                return res;
            started = true;
            t0 = Clock::now();
            firstMs = f.timestamp_ms;
            s = track::seedSample(req.seed, f.timestamp_ms);
        } else {
            s = tracker.step(f.rgba.data(), f.width, f.height, &m[0][0], f.timestamp_ms);
        }
        res.samples.push_back(s);
        if (s.good) {
            lostRun = 0;
            lastGood = res.samples.size() - 1;
        } else {
            ++lostRun;
        }
        ++frames;
        // Like playback: hold each frame back to its time in the footage.
        if (req.realTime) {
            const auto due = t0 + std::chrono::milliseconds(f.timestamp_ms - firstMs);
            const auto now = Clock::now();
            if (due > now) {
                idleSec += std::chrono::duration<double>(due - now).count();
                std::this_thread::sleep_until(due);
            }
        }
        if (onFrame)
            onFrame(std::move(f), &m[0][0], s);
        if (maxLost && lostRun > maxLost)
            break;
    }
    // Keep the path up to the last verified frame: what follows it is either
    // lost or unconfirmed.
    if (!res.samples.empty()) {
        res.endedLost = lastGood + 1 < res.samples.size();
        res.samples.resize(lastGood + 1);
    }
    const double sec = std::chrono::duration<double>(Clock::now() - t0).count() - idleSec;
    res.fpsAchieved = sec > 0.0 ? frames / sec : 0.0;
    return res;
}

} // namespace subjecttrack
