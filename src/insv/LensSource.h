// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <string>

namespace insv {

// Where a lens's picture comes from: a file and its n-th video stream. One
// file can hold every lens (the X5: streams 0 and 1), or each lens has a file
// of its own (the ONE X2 at 5.7K).
struct LensSource {
    std::string path;
    int stream = 0; // index among the file's video streams
};

} // namespace insv
