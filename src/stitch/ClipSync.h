#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "ClipMeasure.h"   // stitch::MeasureFailure
#include "IStitchSource.h" // stitch::PreviewStitchConfig::LensGuard

// Per-clip video <-> IMU sync fine-tune: stitches a few short windows of the
// clip unstabilized at low resolution and runs insv::SyncEstimator on them.
// Seconds of GPU stitch + feature tracking; run it off the GUI thread. Needs
// OpenCV (built only with insv_optical).

namespace stitch {

struct ClipSyncResult {
    bool ok = false;
    double adjustMs = 0.0; // GyroStabilizer::build(..., syncAdjustSec = adjustMs / 1000)
    int cellsUsed = 0;
    double improvement = 0.0; // 1 - residual(best) / residual(0)
    std::string message;      // why it failed, or a one-line summary
    MeasureFailure failure = MeasureFailure::None; // why not ok
};

// Measures over [trimInMs, trimOutMs] of `clipPath` (up to three ~4 s windows
// spread across it). `cancel` (optional) is polled between frames.
// `lensGuard` selects the stitch's lens profile, as in the preview.
ClipSyncResult MeasureClipSync(const std::string& clipPath, int64_t trimInMs, int64_t trimOutMs,
                               const std::atomic<bool>* cancel = nullptr,
                               PreviewStitchConfig::LensGuard lensGuard =
                                   PreviewStitchConfig::LensGuard::Premium);

} // namespace stitch
