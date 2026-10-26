// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

// A camera's GPS track as the overlays take it, whichever camera recorded
// it: the position updates, timed in the clip's own video time. Each
// camera's reader fills one (for Insta360 files: from ReadInsvGps,
// InsvTrailer.h); the telemetry knows nothing about the file it came from.

namespace insv {

struct GpsFix {
    // Video time of the file: ms after its first frame, negative before it.
    int64_t timeMs = 0;
    double latitude = 0;   // degrees, north positive
    double longitude = 0;  // degrees, east positive
    double speedMps = 0;   // over ground
    // Course over ground, degrees clockwise from north; NaN where the
    // camera doesn't know it (standing still).
    double headingDeg = 0;
    double altitudeM = 0;
};

struct GpsTrack {
    std::vector<GpsFix> fixes; // in time order, one per position update
};

} // namespace insv
