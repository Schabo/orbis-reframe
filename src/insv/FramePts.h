// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>

namespace insv {

// A decoded frame's presentation time, twice. `ms` is rounded to the
// millisecond, the unit of seeks, trims, keyframes and the timeline. `us` is
// the stream's time to the microsecond, for whatever turns the picture with
// time (the stabilizer's correction, the capture-time correction, the sync
// fine-tune): at 29.97 fps the rounded time is up to 0.37 ms off, and a
// correction taken there is off by the camera's rotation in that time.
struct FramePts {
    int64_t ms = 0;
    int64_t us = 0;
    double sec() const { return static_cast<double>(us) * 1e-6; }
};

} // namespace insv
