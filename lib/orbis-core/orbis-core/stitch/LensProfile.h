// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <string>

namespace orbiscore {

// How a lens, bare or behind an accessory (a lens guard, a dive case), maps
// the off-axis angle onto its sensor; ApplyLensProfile (FisheyeProjection.h)
// corrects the stitch with the accessory's against the bare lens's. A
// camera's reader fills it from its file. `poly` maps the off-axis angle in
// degrees to the image height on the sensor in mm (sum poly[i] * theta^i),
// `fov_deg` is the usable field of view. Insta360 writes a table of them
// (0x101 field 145, docs/formats/insta360-x5.md §5.4); names seen on the X5:
// "bare", "ProtectorA", "ProtectorS", "ProtectorAS", "InvisibleDiveWater",
// "InvisibleDiveAir".
struct LensProfile {
    std::string name;
    double poly[6] = {0, 0, 0, 0, 0, 0};
    double fov_deg = 0; // 0 = not listed
};

} // namespace orbiscore
