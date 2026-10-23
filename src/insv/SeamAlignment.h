// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "FisheyeProjection.h"
#include "Image.h"
#include "SeamCorrection.h"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

// Seam alignment fine-tune: fits a small correction of the lens calibration
// (StitchParams::seam, insv::SeamCorrection) so the two lenses agree across
// the stitch seam on far content. Measured from the video, like the gyro
// sync fine-tune.
//
// What's left after the trailer calibration (and the lens-guard profile) is
// a fraction of a degree, and of three kinds: a turn of lens 1 (the X2 has
// ~0.5 deg of tilt), a shift of its principal point (an offset along the
// seam that changes with the azimuth like cos / sin -- a rotation can't make
// that), and an offset across the seam that is the same all round (the two
// lenses disagree on the image radius at ~90 deg off axis: a radial scale).
//
// Method: sample a strip around the seam circle from both lenses (grey,
// half resolution), and for each textured 5-deg block find the shift of lens
// 1's strip that matches lens 0's best, with its uncertainty (the NCC peak's
// curvature: an edge pins down only the direction across it). Then a
// weighted least-squares fit of the correction's six numbers to the shifts,
// through the stitcher's own projection (numerical derivatives), re-measured
// over a few rounds. Parallax only ever shifts the match one way (across the
// seam, as for something nearer than infinity), so a block whose shift goes
// further that way than the fit explains gives up its across-the-seam part
// (one-sided), and the shift along the seam -- which parallax can't touch --
// carries the principal point. A weak pull towards no correction keeps the
// directions the clip's blocks don't pin down where they are.
// Dependency-free (insv lib).

namespace insv {

struct SeamAlignmentResult {
    bool ok = false;
    SeamCorrection correction;
    // RMS offset between the lenses over the textured blocks, degrees, with
    // what parallax explains left out: before (no correction) and after.
    double offsetBeforeDeg = 0;
    double offsetAfterDeg = 0;
    int frames = 0;
    int blocks = 0; // block measurements the last round used
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
// with (params.seam is ignored -- the fit starts from none). Needs the
// extended MEI calibration. `cancel` (optional) is polled between steps.
SeamAlignmentResult EstimateSeamAlignment(const InsvCalibration& cal, const StitchParams& params,
                                          int streamW, int streamH,
                                          const std::vector<SeamFrame>& frames,
                                          const std::atomic<bool>* cancel = nullptr);

// The RMS offset between the lenses (as SeamAlignmentResult::offsetAfterDeg)
// with the stitch as `params` has it, params.seam included; < 0 when no block
// has enough detail. For checking a correction on other frames.
double MeasureSeamOffset(const InsvCalibration& cal, const StitchParams& params, int streamW,
                         int streamH, const std::vector<SeamFrame>& frames);

} // namespace insv
