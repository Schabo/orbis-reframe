#pragma once

#include "Title.h" // title::Units

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>

// A gauge: a value from the clip's telemetry drawn as an instrument over the
// output -- a dial, a bar, a heading tape, a lean dial, a G circle or the
// route -- a clip on an overlay track with no file behind it, like a title.
// It is rendered into a transparent picture (src/ui/GaugeRender) and placed
// like a title: the look's `width` is the gauge's height as a fraction of
// the output height, so its shape stays its own at any output size. Its
// value at each moment comes from the same clock as a title's GPS and
// motion fields (title::Clock::gpsAt, motionAt); a route draws the
// timeline's GPS track (title::Clock::route), around the camera or whole.
namespace gauge {

enum class Kind {
    Dial, // round, a needle over a scale, the number in the middle
    Bar,  // the number, a bar filling along a scale under it
    Tape, // the heading: a compass strip sliding under a marker
    Lean, // the lean: a half dial, upright at the top, the degrees in the middle
    GForce, // the acceleration: a dot in a circle, across and along the travel
    Route,  // the trip's path seen from above, north up, a dot where it is
};
// What a dial or a bar shows; the other kinds show one thing each.
enum class Reads { Speed, Altitude };
// How a route is seen: the road around the camera turning with it, the way
// it travels up, a little behind and some time ahead; or the whole trip,
// north up.
enum class RouteView { Ahead, Trip };

struct Style {
    Kind kind = Kind::Dial;
    Reads reads = Reads::Speed;
    title::Units units;
    // The scale's ends in the unit it shows (km/h or mph, metres or feet);
    // a value past either end stays at it. A lean dial's and a G circle's
    // scale is the same either side of 0 (degrees, g): min is -max. Not
    // used by a tape or a route.
    double min = 0.0;
    double max = 200.0;
    RouteView routeView = RouteView::Ahead;
    // How far ahead a route ahead looks, in seconds of the timeline.
    double aheadS = 90.0;
    uint32_t accent = 0xff9f1c; // the needle, the bar, the tape's marker
    uint32_t face = 0x000000;   // behind it
    float faceOpacity = 0.5f;
    uint32_t text = 0xffffff;   // the scale and the numbers
    QString font = QStringLiteral("Sans Serif");

    static constexpr double kLimit = 100000.0; // either end, either sign
    static constexpr double kMinSpan = 1.0;
    // A lean dial's scale either side, degrees; a G circle's, g.
    static constexpr double kMinLeanDeg = 5.0, kMaxLeanDeg = 90.0;
    static constexpr double kMinG = 0.1, kMaxG = 5.0;
    static constexpr double kMinAheadS = 30.0, kMaxAheadS = 300.0;
    // How far back a route ahead shows the road ridden.
    static constexpr double kBehindS = 20.0;

    // Is its scale the same either side of 0?
    bool symmetric() const { return kind == Kind::Lean || kind == Kind::GForce; }

    Style clamped() const
    {
        Style s = *this;
        s.accent &= 0xffffff;
        s.face &= 0xffffff;
        s.text &= 0xffffff;
        s.faceOpacity = std::clamp(std::isfinite(s.faceOpacity) ? s.faceOpacity : 0.5f, 0.0f, 1.0f);
        s.aheadS = std::clamp(std::isfinite(s.aheadS) ? s.aheadS : 90.0, kMinAheadS, kMaxAheadS);
        if (s.symmetric()) {
            const bool lean = s.kind == Kind::Lean;
            const double lo = lean ? kMinLeanDeg : kMinG, hi = lean ? kMaxLeanDeg : kMaxG;
            s.max = std::clamp(std::isfinite(s.max) ? s.max : hi, lo, hi);
            s.min = -s.max;
            return s;
        }
        s.min = std::clamp(std::isfinite(s.min) ? s.min : 0.0, -kLimit, kLimit - kMinSpan);
        s.max = std::clamp(std::isfinite(s.max) ? s.max : s.min + 100.0, s.min + kMinSpan, kLimit);
        return s;
    }
    bool operator==(const Style &) const = default;
};

// Does a gauge of this kind read the clip's motion (lean, G-forces), not
// its GPS?
inline bool readsMotion(Kind k)
{
    return k == Kind::Lean || k == Kind::GForce;
}

// The picture's width over its height.
inline double aspect(Kind k)
{
    switch (k) {
    case Kind::Dial: return 1.0;
    case Kind::Bar: return 3.0;
    case Kind::Tape: return 3.5;
    case Kind::Lean: return 1.8;
    case Kind::GForce: return 1.0;
    case Kind::Route: return 1.0;
    }
    return 1.0;
}

// A scale that suits what it reads in that unit: up to 200 km/h or 120 mph,
// 1000 m or 3000 ft; a lean dial 60 degrees either way, a G circle 1 g.
struct Range {
    double min = 0.0;
    double max = 0.0;
};
inline Range defaultRange(Kind kind, Reads reads, title::Units u)
{
    if (kind == Kind::Lean)
        return {-60.0, 60.0};
    if (kind == Kind::GForce)
        return {-1.0, 1.0};
    if (reads == Reads::Speed)
        return {0.0, u.speed == title::SpeedUnit::Mph ? 120.0 : 200.0};
    return {0.0, u.altitude == title::AltitudeUnit::Feet ? 3000.0 : 1000.0};
}

// A gauge's height when added, as a fraction of the output height.
constexpr float kDefaultSize = 0.25f;
// The plain gauge's look when added: lower right, out of a title's way.
inline inset::Look defaultLook()
{
    inset::Look l;
    l.centerX = 0.85f;
    l.centerY = 0.75f;
    l.width = kDefaultSize;
    return l;
}

} // namespace gauge
