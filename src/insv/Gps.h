// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <utility>
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
    // The fixes are timed by the camera's clock stamp of its first frame
    // (the X5's field 98.3, docs/formats/insta360-x5.md §8), which sits up
    // to ~2 s off the video and drifts from the GPS clock:
    // camera::ReadClipTelemetry then lines them up against the camera's own motion
    // (AlignGpsByAcceleration) where the clip lets it.
    bool clockStamped = false;
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

// How far a GPS track runs behind the video: a fix timed at video ms `t`
// happened shiftAt(t) seconds earlier.
struct GpsClockFit {
    // (video s, shift s) in time order, at least one; straight between
    // them, held beyond the ends.
    std::vector<std::pair<double, double>> shifts;
    double shiftAt(double videoSec) const;
    int64_t videoMs(int64_t timeMs) const;
};

// A GPS track timed by a clock stamp (GpsTrack::clockStamped) lined up with
// the video by what the vehicle did: the change of the fixes' speed matched
// against the camera's acceleration along its travel, `forwardG` (in g, one
// value per `stepSec` of video from 0 s, NaN where unknown;
// MotionTrack::forwardG, Motion.h). On the X5 samples the GPS runs 0.5-2 s
// behind the video, and on a 21 min ride by 2.1 s at the start and 1.5 s
// later: the match is made over each ~2 min of the track, a stretch that
// matches far from the others is left out. nullopt when no stretch matches
// clearly (a steady speed, a stop, too little track, no shift within
// +-3 s).
std::optional<GpsClockFit> AlignGpsByAcceleration(const GpsTrack& track,
                                                  const std::vector<float>& forwardG,
                                                  double stepSec);

} // namespace insv
