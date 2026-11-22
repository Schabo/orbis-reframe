// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>

namespace orbiscore {

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

// The one rounding between the two: a frame at `us` is at MsOfUs(us) ms,
// rounded half up. Everything that reports a frame time in ms rounds this
// way, so a time a frame was reported at finds that frame again.
inline int64_t MsOfUs(int64_t us)
{
    return us >= 0 ? (us + 500) / 1000 : -((-us - 500 + 999) / 1000);
}

// "The first frame at or after `ms`" is the first frame whose time rounds
// to `ms` or later (MsOfUs): the first at or after this many us. Exact --
// no slack -- so the frame before, a hair under the ms, isn't taken for it.
inline int64_t FirstUsOfMs(int64_t ms) { return ms * 1000 - 500; }

} // namespace orbiscore
