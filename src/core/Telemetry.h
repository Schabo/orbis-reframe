#pragma once

#include <Gps.h> // insv::GpsTrack

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// A clip's GPS telemetry at any moment of its video, for the overlays. The
// camera fixes its position about once a second; between fixes every value
// follows a smooth curve through them (monotone cubic: it passes through
// each fix and never swings past its neighbours, so a steady speed doesn't
// wobble and a stop doesn't dip below zero). Where fixes are more than
// kMaxGapMs apart -- the signal was lost, or the GPS source wasn't connected
// yet -- there is no data. Each stretch's ends hold for kHoldMs, as far as a
// bridged gap reaches from either side, so a track whose record stops a
// second before the video (as the X5's can) still covers its clip.
class Telemetry {
public:
    struct Values {
        double latitude = 0.0;  // degrees, north positive
        double longitude = 0.0; // degrees, east positive, -180..180
        double speedMps = 0.0;  // over ground, >= 0
        // Course over ground, 0..360 clockwise from north. Through a stop
        // (where the camera doesn't know it) the last heading it had, the
        // way the vehicle still points; nullopt only if the whole stretch
        // has none.
        std::optional<double> headingDeg;
        double altitudeM = 0.0;
    };

    static constexpr int64_t kMaxGapMs = 3000;
    static constexpr int64_t kHoldMs = kMaxGapMs / 2;

    Telemetry() = default;
    explicit Telemetry(const insv::GpsTrack &track);

    bool isEmpty() const { return m_points.empty(); }
    // The values at `sourceMs` of the clip's video, nullopt where there's no
    // data.
    std::optional<Values> at(double sourceMs) const;

private:
    // Latitude, longitude, speed, heading and altitude, the angles unwrapped
    // so that each step is the short way round.
    static constexpr int kChannels = 5;
    struct Point {
        double timeMs = 0.0;
        std::array<double, kChannels> value{};
        std::array<double, kChannels> slope{}; // per ms
        bool segmentEnd = false; // the next point is past a gap
        bool hasHeading = true;
    };

    static Values valuesOf(const std::array<double, kChannels> &v, bool hasHeading);

    std::vector<Point> m_points;
};
