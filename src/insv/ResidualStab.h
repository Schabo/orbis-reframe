// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// Residual 2-D stabilization. The gyro path (insv::GyroStabilizer)
// counter-rotates the sphere by the rotation the .insv IMU actually recorded --
// on rigidly-mounted motorcycle footage that is tiny (~0.1 deg/frame). What is
// left is apparent bounce of distant content (road, horizon, scenery): a
// structured 2-D drift of the reframed forward view, part gyro error and part
// near-rigid parallax. This estimates that drift from the image and high-passes
// it into a small per-frame yaw/pitch correction to ADD to the reframe pose,
// on top of (after) the gyro de-rotation. It does NOT fix near-field parallax
// -- that needs depth + view synthesis (a stretch goal).
//
// The estimator (ResidualPanEstimator) needs OpenCV and lives in insv_optical.
// ResidualTrack is the OpenCV-free result that playback / export sample -- it
// is header-only so consumers need not link insv_optical to apply a track.
//
// Prototyped in tools/optflow_stab_spike --render derotate2d; the productised
// path drops the (ruled-out) image-based rotation solve and works on the
// gyro-de-rotated feature directions instead.

namespace insv {

// Per-frame residual correction, radians, to ADD to the reframe pose's yaw /
// pitch at a given video source-time. Built by ResidualPanEstimator::finish();
// held by the app and sampled at playback / export time (cheap).
struct ResidualTrack {
    std::vector<double> t;    // seconds, video-source time, strictly increasing
    std::vector<float> dyaw;  // radians, add to pose yaw at t[i]
    std::vector<float> dpitch;

    bool empty() const { return t.size() < 2; }

    // Linear interpolation; holds the endpoints outside [t.front(), t.back()].
    // Writes (0, 0) when empty().
    void sample(double tSec, float& dYaw, float& dPitch) const
    {
        dYaw = 0.0f;
        dPitch = 0.0f;
        if (empty())
            return;
        if (tSec <= t.front()) {
            dYaw = dyaw.front();
            dPitch = dpitch.front();
            return;
        }
        if (tSec >= t.back()) {
            dYaw = dyaw.back();
            dPitch = dpitch.back();
            return;
        }
        const auto it = std::upper_bound(t.begin(), t.end(), tSec);
        const size_t i1 = static_cast<size_t>(it - t.begin());
        const size_t i0 = i1 - 1;
        const double span = t[i1] - t[i0];
        const double f = span > 1e-12 ? (tSec - t[i0]) / span : 0.0;
        dYaw = static_cast<float>(dyaw[i0] + (dyaw[i1] - dyaw[i0]) * f);
        dPitch = static_cast<float>(dpitch[i0] + (dpitch[i1] - dpitch[i0]) * f);
    }
};

} // namespace insv
