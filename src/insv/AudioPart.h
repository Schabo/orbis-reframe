// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace insv {

// Where a clip's sound comes from: a file whose audio streams play from
// `startUs` in clip time. A recording the camera split over several files
// (ClipPart) has one per file, played back to back; any other file is one
// part at 0.
struct AudioPart {
    std::string path;
    int64_t startUs = 0;
};

} // namespace insv
