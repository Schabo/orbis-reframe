// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

namespace insv {

// Per-clip correction of the trailer's lens calibration, measured from the
// video so the two lenses agree across the stitch seam
// (insv::EstimateSeamAlignment). Plain data, no dependencies: the project
// model stores it per source file.
struct SeamCorrection {
    // Lens 1's extra yaw (about x), pitch (about y) and roll (about z) in our
    // lens frame, degrees.
    double lens1RotDeg[3] = {0, 0, 0};

    bool operator==(const SeamCorrection&) const = default;
};

} // namespace insv
