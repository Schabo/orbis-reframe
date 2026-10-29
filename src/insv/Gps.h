// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
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

// A speed over ground at a moment, for lining a GPS track up with the video.
struct SpeedSample {
    double timeSec = 0;
    double speedMps = 0;
};

// Where a file stamps its GPS fixes on a clock of their own and says nothing
// that ties that clock to the video's, the fixes' speeds can be matched
// against a speed the camera timed in video time (a drone's flight
// controller's velocity). `fixes` are timed on the GPS clock, `reference` in
// video time, both in time order. Returns the shift that turns a fix's time
// into video time, or nullopt when the match doesn't pin it down: too few
// fixes overlap the reference, the speed hardly changes (a hover), the
// speeds don't agree anywhere, or another shift fits almost as well.
std::optional<double> AlignGpsBySpeed(const std::vector<SpeedSample>& fixes,
                                      const std::vector<SpeedSample>& reference);

} // namespace insv
