#pragma once

#include <Motion.h> // insv::MotionTrack

#include <optional>

// A clip's lean and G-forces at any moment of its video, for the overlays:
// insv::BuildMotion's track (100 values a second, already smoothed), read
// between its values on a straight line. Where the IMU has no samples there
// is no data; the track's ends hold for one step, so the last frame of a
// video the IMU stops a sample short of is still covered.
class MotionTelemetry {
public:
    struct Values {
        double leanDeg = 0.0;  // positive leaning right, 0 upright
        double lateralG = 0.0; // across the travel, positive toward the right
        double forwardG = 0.0; // along the travel, positive speeding up
        // Was the direction of travel found (insv::MotionTrack::travelFit)?
        // Where the clip hardly turns it wasn't, and the G-forces are along
        // and across the camera's forward, which may point any way: they
        // aren't shown then.
        bool travelKnown = true;
    };

    MotionTelemetry() = default;
    explicit MotionTelemetry(insv::MotionTrack track);

    bool isEmpty() const { return m_track.empty(); }
    // The values at `sourceMs` of the clip's video, nullopt where there's no
    // data.
    std::optional<Values> at(double sourceMs) const;

private:
    // Sample `i`'s values, nullopt where it has none.
    std::optional<Values> sample(size_t i) const;

    insv::MotionTrack m_track;
};
