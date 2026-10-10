// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "FisheyeProjection.h"
#include "Image.h"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

// Seam alignment fine-tune: fits a small extra rotation of lens 1
// (StitchParams::lens1AdjustDeg) so the two lenses agree across the stitch
// seam.
//
// With the guard profile and the right extrinsic signs, the sample clips still
// show a consistent ~0.35-0.6 deg lens-1 yaw residual -- visible as doubled
// window frames on a building on the seam. It is the same on every clip and
// scene (not parallax), so it's a camera/guard property the trailer calibration
// doesn't carry. Measured from the video, like the gyro sync fine-tune.
//
// Method: sample a thin strip around the seam circle (|e| <= 2.5 deg of the
// lens-0 90 deg line) from both lenses in grey, score it by the mean NCC of
// textured 10-deg azimuth blocks (textureless and parallax-dominated blocks
// wash out), averaged over the frames, and coordinate-descend yaw / pitch /
// roll from 0 in shrinking steps. Dependency-free (insv lib).

namespace insv {

struct SeamAlignmentResult {
    bool ok = false;
    double adjustDeg[3] = {0, 0, 0}; // lens-1 yaw, pitch, roll to add
    double scoreBefore = 0;          // mean block NCC at no adjustment
    double scoreAfter = 0;           // ... at adjustDeg
    int frames = 0;
    std::string message; // why it failed, or a one-line summary
};

// A lens pair reduced to what the estimator samples: grey, half resolution.
struct SeamFrame {
    int w = 0, h = 0; // of the half-res grey planes
    std::vector<uint8_t> grey[2];
};

// Rounds `ms` to the nearest X5 keyframe time (GOP 60 frames), so a seek there
// decodes one frame instead of up to a GOP. Any frame serves the estimator.
int64_t SnapToKeyframeMs(int64_t ms, double fps);

// Converts a decoded lens pair (full-res RGB) to a SeamFrame.
SeamFrame MakeSeamFrame(const Image& lens0, const Image& lens1);

// `cal` / `params` / `streamW` / `streamH` are what the stitch is configured
// with (params.lens1AdjustDeg is ignored -- the fit starts from zero). Needs
// the extended MEI calibration. `cancel` (optional) is polled between steps.
SeamAlignmentResult EstimateSeamAlignment(const InsvCalibration& cal, const StitchParams& params,
                                          int streamW, int streamH,
                                          const std::vector<SeamFrame>& frames,
                                          const std::atomic<bool>* cancel = nullptr);

} // namespace insv
