#include "ClipSync.h"

#include "NativeStitchSource.h"

#include <Calibration.h>
#include <ClipMetadata.h>
#include <GyroStabilizer.h>
#include <SyncEstimator.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace stitch {

ClipSyncResult MeasureClipSync(const std::string& clipPath, int64_t trimInMs, int64_t trimOutMs,
                               const std::atomic<bool>* cancel,
                               PreviewStitchConfig::LensGuard lensGuard)
{
    ClipSyncResult out;
    const auto fail = [&out](MeasureFailure why, std::string message) {
        out.failure = why;
        out.message = std::move(message);
        return out;
    };
    // The file and its stitch first: one that can't be read has no IMU
    // either, and should say so as unreadable.
    std::string err;
    insv::InsvCalibration calib;
    insv::StitchParams params;
    if (const MeasureFailure why = PrepareClipMeasurement(clipPath, lensGuard, calib, params, err);
        why != MeasureFailure::None)
        return fail(why, err);
    const auto imu = camera::ReadClipImu(clipPath);
    if (!imu)
        return fail(MeasureFailure::NoGyro, "no IMU track");
    // The raw orientation track, synced exactly as stabilization would be
    // (before any fine-tune); the estimate is relative to it.
    insv::GyroStabilizer gyro;
    gyro.build(*imu, insv::StabConfig::preset(insv::StabMode::LeanFollow));
    if (!gyro.ready())
        return fail(MeasureFailure::NoGyro, "gyro track not usable");

    // Low-res, unstabilized, no capture-time correction: the estimator models
    // the rolling shutter itself.
    NativeStitchSource src;
    PreviewStitchConfig cfg;
    cfg.width = 1024;
    cfg.height = 512;
    cfg.lensGuard = lensGuard;
    if (!src.Open(clipPath, cfg))
        return fail(MeasureFailure::Unreadable, src.GetLastError());

    // Up to three ~4 s windows spread over the trimmed range (one shorter
    // window for a short clip): shake and texture vary along a ride.
    constexpr int64_t kWindowMs = 4000;
    const int64_t span = std::max<int64_t>(0, trimOutMs - trimInMs);
    std::vector<int64_t> starts;
    if (span >= 4 * kWindowMs) {
        for (double f : {0.25, 0.5, 0.75})
            starts.push_back(trimInMs + static_cast<int64_t>(f * span) - kWindowMs / 2);
    } else {
        starts.push_back(trimInMs);
    }
    const int64_t winMs = std::min(kWindowMs, span);

    insv::SyncEstimator est;
    std::vector<uint8_t> gray;
    for (const int64_t start : starts) {
        if (!src.SeekTo(start))
            return fail(MeasureFailure::Unreadable, src.GetLastError());
        est.beginWindow();
        for (;;) {
            if (cancel && cancel->load())
                return fail(MeasureFailure::Cancelled, "cancelled");
            const Frame f = src.ReadFrame();
            if (f.rgba.empty() || f.timestamp_ms > start + winMs)
                break;
            gray.resize(static_cast<size_t>(f.width) * f.height);
            const uint8_t* p = f.rgba.data();
            for (size_t i = 0; i < gray.size(); ++i, p += 4)
                gray[i] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            est.addFrame(gray.data(), f.width, f.height, f.timestamp_ms / 1000.0);
        }
    }

    const auto r = est.estimate(gyro, gyro.readoutSec(), [&src](const double d[3], double& row) {
        int lens = 0;
        return src.SensorRowOf(d, lens, row);
    });
    out.cellsUsed = r.cellsUsed;
    out.improvement = r.improvement;
    if (!r.ok) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "no clear sync minimum (%d usable cells, %d intervals)",
                      r.cellsUsed, r.intervals);
        return fail(MeasureFailure::TooLittleDetail, buf);
    }
    out.ok = true;
    out.adjustMs = r.adjustSec * 1000.0;
    char buf[128];
    std::snprintf(buf, sizeof buf, "sync fine-tune %+.1f ms (%d cells, %.0f%% lower residual)",
                  out.adjustMs, r.cellsUsed, r.improvement * 100.0);
    out.message = buf;
    return out;
}

} // namespace stitch
