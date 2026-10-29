#pragma once

#include "../core/Gauge.h"
#include "../core/MotionTelemetry.h"
#include "../core/Telemetry.h"
#include "../core/TitleClock.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// A gauge as a picture: drawn with QPainter into a transparent RGBA image
// (premultiplied alpha), which playback and the export then place and blend
// like a title's. Thread-safe: playback's decoder thread and the export
// render gauges too.
namespace gaugerender {

// What a gauge shows at one moment. Its number is the value rounded as a
// title's field rounds it, so the two agree; its needle, bar, tape or dot
// sits at `position` (and `position2`), the value rounded only as finely as
// the picture can show (quantized), so a picture can be reused.
struct Reading {
    bool hasValue = false; // false: no data, the gauge drawn empty with "--"
    // As written: whole km/h, metres, degrees (a lean's sign its side),
    // hundredths of a g.
    int number = 0;
    // In the gauge's unit: degrees for a tape or a lean dial, g across the
    // travel for a G circle; for a route how far along it the ride is, in
    // the route's square (Route::Point::along).
    double position = 0.0;
    double position2 = 0.0; // a G circle's g along the travel
    bool operator==(const Reading &) const = default;
};

// The trip a route gauge draws: every timeline clip's GPS track
// (title::Clock::route), projected flat onto a square seen from above,
// north up, fitted into it with its shape kept; 0..1 each way, y down.
struct Route {
    struct Point {
        int64_t timelineMs = 0;
        double x = 0.0, y = 0.0;
        double along = 0.0; // the length of the route up to here
    };
    std::vector<std::vector<Point>> stretches; // with a gap or a cut between
    // Tells routes apart in the picture cache: the same for the same route.
    uint64_t key = 0;

    bool empty() const { return stretches.empty(); }
    // How far along the route the ride is at `timelineMs`: in a gap or a
    // cut where the last stretch ended, before the start 0.
    double alongAt(int64_t timelineMs) const;
};
// The route `clock`'s main-track clips ride, null when none has GPS.
std::shared_ptr<const Route> routeOf(const title::Clock &clock);

// What a gauge reads at one moment: the GPS and motion values there (none:
// no data) and, for a route, the timeline time.
struct Moment {
    std::optional<Telemetry::Values> gps;
    std::optional<MotionTelemetry::Values> motion;
    int64_t timelineMs = 0;
};

// The reading of `style` at `moment` on a picture `heightPx` high: a dial
// or a bar its speed or altitude in its unit, a tape the heading, a lean
// dial the lean, a G circle the G-forces (none where the direction of
// travel isn't known), a route how far along `route` the ride is (the dot
// shown where there's GPS).
Reading readingOf(const gauge::Style &style, const Moment &moment, int heightPx,
                  const Route *route = nullptr);
// ... from GPS values alone.
inline Reading readingOf(const gauge::Style &style, const std::optional<Telemetry::Values> &gps,
                         int heightPx)
{
    return readingOf(style, Moment{gps, std::nullopt, 0}, heightPx);
}

struct Picture {
    std::shared_ptr<const std::vector<uint8_t>> rgba; // width * height * 4
    int width = 0;
    int height = 0;

    bool empty() const { return !rgba; }
};

// The gauge `heightPx` high (as wide as its kind's aspect makes it) showing
// `reading` (a route gauge `route`, none: no GPS anywhere); one picture per
// (style, height, reading, route) kept for reuse.
Picture render(const gauge::Style &style, int heightPx, const Reading &reading,
               const Route *route = nullptr);
// The same if it has been made and is still kept, else an empty picture.
Picture cached(const gauge::Style &style, int heightPx, const Reading &reading,
               const Route *route = nullptr);

} // namespace gaugerender
