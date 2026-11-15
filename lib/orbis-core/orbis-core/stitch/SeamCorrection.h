// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

namespace insv {

// Per-clip correction of the trailer's lens calibration, measured from the
// video so the two lenses agree across the stitch seam
// (insv::EstimateSeamAlignment). Plain data, no dependencies: the project
// model stores it per source file.
//
// Only the relative fit of the two lenses can be seen at the seam, so the
// rotation and centre go on lens 1 and the scale on both.
struct SeamCorrection {
    // Lens 1's extra yaw (about x), pitch (about y) and roll (about z) in our
    // lens frame, degrees.
    double lens1RotDeg[3] = {0, 0, 0};
    // Lens 1's principal-point offset in the calibration's image axes (u
    // right, v down), as a fraction of the decoded lens image width, so one
    // value serves every resolution of the clip's mode.
    double lens1Centre[2] = {0, 0};
    // Image-radius scale of both lenses (their focal lengths), 1 = as
    // calibrated. Moves the two pictures towards or away from each other
    // across the seam where a rotation can't.
    double radialScale = 1.0;

    bool operator==(const SeamCorrection&) const = default;
};

// The estimator that measured a stored correction. A correction from an
// older one (rotation only) is measured again rather than used.
constexpr int kSeamCorrectionMethod = 2;

} // namespace insv
