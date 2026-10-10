// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "ResidualStab.h" // insv::ResidualTrack

#include <cstdint>
#include <memory>

// Estimator half of residual 2-D stabilization (see ResidualStab.h). Needs
// OpenCV (feature tracking + optical flow) -> compiled into insv_optical.
// Pimpl'd so this header pulls in no OpenCV -- consumers that only apply a
// ResidualTrack include ResidualStab.h instead.

namespace insv {

class ResidualPanEstimator {
public:
    struct Params {
        int maxFeatures = 1200;
        double qualityLevel = 0.005;
        double minDistancePx = 7.0;
        double fwdBwdTolPx = 1.0;   // forward-backward LK consistency
        double poleMarginFrac = 0.14; // ignore the distorted equirect caps
        // Only features whose de-rotated frame-N+1 direction points into the
        // forward hemisphere (dir.z < forwardZMax) feed the pan estimate --
        // that is the part of the sphere a reframe actually shows.
        double forwardZMax = -0.5;
        int minMatches = 10; // below this, the interval contributes no delta
        double maxPanPerFrameRad = 0.0175; // ~1 deg; clamp implausible medians
    };

    ResidualPanEstimator();
    explicit ResidualPanEstimator(const Params& params);
    ~ResidualPanEstimator();
    ResidualPanEstimator(const ResidualPanEstimator&) = delete;
    ResidualPanEstimator& operator=(const ResidualPanEstimator&) = delete;

    // Feed consecutive equirect frames in play order. `gray`: 8-bit single
    // channel, w x h, a 2:1 equirect (any analysis resolution). `gyro`:
    // row-major 3x3 de-rotation for this frame's time (identity if no gyro
    // track) -- the same matrix insv::GyroStabilizer::correctionMatrix returns.
    // The first call just stores the frame; each later call measures the
    // inter-frame forward-hemisphere pan of the gyro-de-rotated sphere and
    // accumulates it.
    void addFrame(const uint8_t* gray, int w, int h, double tSec, const float gyro[3][3]);

    int intervalCount() const;      // measured frame-to-frame intervals
    double medianMatches() const;   // features surviving per interval (diagnostic)

    // The raw (un-smoothed) accumulated apparent forward-view pan path, radians,
    // one entry per measured interval. Same length as times(). For diagnostics
    // and PR2's cache.
    const std::vector<double>& accumulatedYaw() const;
    const std::vector<double>& accumulatedPitch() const;
    const std::vector<double>& times() const;

    // High-pass the accumulated pan path into a correction track: for each
    // interval, dyaw/dpitch = smoothed(path) - path, where the smoothing window
    // comes from strength01 via insv::leanWindowFromStrength (0 => ~0.2 s, barely
    // any correction; 1 => ~2.5 s, heavy). `fps` sets the window length in
    // samples. Returns an empty track if fewer than 2 intervals were measured.
    ResidualTrack finish(float strength01, double fps) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace insv
