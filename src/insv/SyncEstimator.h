// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// Per-clip fine-tuning of the video <-> IMU sync from the video itself. The
// trailer only pins the sync to a few ms per clip, but the stick mount's ~11 Hz
// shake needs ~1 ms: 1.5 ms of error doubles the residual jitter.
//
// Method: on unstabilized low-res equirect frames, track features and take
// the median frame-to-frame flow in a grid of cells. For each cell, predict
// the flow the gyro alone would cause (a pure camera rotation), with every
// sensor row captured at its own rolling-shutter time. Then grid-search the
// sync adjustment that best explains the measured flow. Both flows are
// high-passed, so slow forward-motion parallax drops out, and only cells the
// gyro explains well take part (the rider / bike move with the camera,
// and near objects show parallax). Needs OpenCV -> compiled into
// insv_optical; pimpl'd so this header pulls in none.

namespace insv {

class GyroStabilizer;

class SyncEstimator {
public:
    struct Params {
        int cellsX = 16;           // equirect grid
        int cellsY = 8;
        double latMaxDeg = 65.0;   // skip the distorted caps
        int maxFeatures = 4000;
        int minFeaturesPerCell = 5;
        double searchMs = 12.0;    // adjustment searched over +-searchMs
        double stepMs = 0.5;
        int highPassFrames = 15;   // moving-average window removed from both flows
        double goodRatio = 0.7;    // cell kept if best residual / raw flow < this
        int minIntervals = 40;     // valid intervals a cell needs
        int minCells = 12;         // cells needed for a result
    };

    // Sensor row of a sphere direction (equirect frame, unit): fraction of the
    // fisheye height, 0 = first row read. False where unknown (seam / outside)
    // -- such cells are skipped. stitch::NativeStitchSource::SensorRowOf fits.
    using RowFn = std::function<bool(const double dir[3], double& rowFrac)>;

    struct Result {
        bool ok = false;
        double adjustSec = 0.0;  // add to the stabilizer's derived offset
        int cellsUsed = 0;
        int intervals = 0;
        double improvement = 0.0; // 1 - residual(best) / residual(0)
        std::vector<double> curve; // normalized residual per step, -searchMs..+searchMs
    };

    SyncEstimator();
    explicit SyncEstimator(const Params& params);
    ~SyncEstimator();
    SyncEstimator(const SyncEstimator&) = delete;
    SyncEstimator& operator=(const SyncEstimator&) = delete;

    // Start a new contiguous run of frames (after a seek).
    void beginWindow();
    // Feed consecutive frames of the current window: 8-bit gray 2:1 equirect,
    // unstabilized, at its video source time.
    void addFrame(const uint8_t* gray, int w, int h, double tSec);

    // `gyro` must be built from the clip with the sync to be refined (its
    // rawOrientation() is sampled at video time + adjustment + row time);
    // `readoutSec` is the sensor readout (GyroStabilizer::readoutSec()).
    Result estimate(const GyroStabilizer& gyro, double readoutSec, const RowFn& rowOf) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace insv
