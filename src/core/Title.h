#pragma once

#include "InsetGeometry.h"

#include <QString>

#include <algorithm>
#include <cstdint>

// A title: text drawn over the output, a clip on an overlay track that has no
// file behind it. The text is rendered into a transparent picture
// (src/ui/TitleRender) and placed like any overlay (inset::Look: centre,
// opacity, fades, keyframes); for a title the look's `width` is the text size
// instead -- the font's pixel size as a fraction of the output height -- so
// the box follows the text, and editing the text doesn't change its size.
//
// Every length in the style is a fraction of the font's pixel size (em), so
// the picture looks the same at any size: the view's and the export's match.
namespace title {

// How the lines line up, and which point of the text the look's centreX is:
// a left-aligned title keeps its left edge put as its text changes (see
// drawnLook). Vertically the look's centre is always the text's middle.
enum class Align { Left, Centre, Right };

// Drawn round each letter, under the fill: `width` em outside the letter's
// edge.
struct Outline {
    bool on = false;
    float width = 0.05f;
    uint32_t rgb = 0x000000;

    static constexpr float kMaxWidth = 0.3f;
    bool operator==(const Outline &) const = default;
};

// A soft copy of the letters (and their outline) below and to the right.
struct Shadow {
    bool on = false;
    uint32_t rgb = 0x000000;
    float opacity = 0.6f;
    float distance = 0.06f; // em, down and right at 45°
    float blur = 0.06f;     // em, the blur's radius

    static constexpr float kMaxDistance = 0.5f;
    static constexpr float kMaxBlur = 0.5f;
    bool operator==(const Shadow &) const = default;
};

// A filled box behind the text, `padding` em out from the lines.
struct Box {
    bool on = false;
    uint32_t rgb = 0x000000;
    float opacity = 0.6f;
    float padding = 0.3f;
    float cornerRadius = 0.0f; // fraction of the box's shorter side, 0..0.5

    static constexpr float kMaxPadding = 1.0f;
    bool operator==(const Box &) const = default;
};

// How a clock field writes the hours (title::withFields): as the system's
// locale does, or always 24-hour / 12-hour.
enum class Hours { System, H24, H12 };

// The units a title's GPS fields write their values in (title::withFields).
// A new title takes the system's (defaultUnits); a plain Style's are metric,
// so a project reads the same on any computer.
enum class SpeedUnit { Kmh, Mph };
enum class AltitudeUnit { Metres, Feet };
struct Units {
    SpeedUnit speed = SpeedUnit::Kmh;
    AltitudeUnit altitude = AltitudeUnit::Metres;
    bool operator==(const Units &) const = default;
};

// A GPS speed in `unit`: under a walking pace -- the GPS's jitter standing
// still stays under it (up to 0.37 m/s on the X5's samples) -- 0.
constexpr double kStillMps = 0.5;
inline double speedIn(double mps, SpeedUnit unit)
{
    return mps < kStillMps ? 0.0 : mps * (unit == SpeedUnit::Mph ? 2.236936 : 3.6);
}
inline double altitudeIn(double metres, AltitudeUnit unit)
{
    return unit == AltitudeUnit::Feet ? metres / 0.3048 : metres;
}
inline const char *unitName(SpeedUnit unit)
{
    return unit == SpeedUnit::Mph ? "mph" : "km/h";
}
inline const char *unitName(AltitudeUnit unit)
{
    return unit == AltitudeUnit::Feet ? "ft" : "m";
}

struct Style {
    QString text;
    QString font = QStringLiteral("Sans Serif"); // family
    bool bold = true;
    bool italic = false;
    uint32_t rgb = 0xffffff;
    Align align = Align::Centre;
    float lineSpacing = 1.0f;   // times the font's own line spacing
    float letterSpacing = 0.0f; // em added after each letter (negative: tighter)
    Outline outline;
    Shadow shadow;
    Box box;
    Hours hours = Hours::System;
    Units units;

    static constexpr float kMinLineSpacing = 0.5f;
    static constexpr float kMaxLineSpacing = 3.0f;
    static constexpr float kMinLetterSpacing = -0.2f;
    static constexpr float kMaxLetterSpacing = 1.0f;

    Style clamped() const
    {
        Style s = *this;
        s.rgb &= 0xffffff;
        s.lineSpacing = std::clamp(s.lineSpacing, kMinLineSpacing, kMaxLineSpacing);
        s.letterSpacing = std::clamp(s.letterSpacing, kMinLetterSpacing, kMaxLetterSpacing);
        s.outline.width = std::clamp(s.outline.width, 0.0f, Outline::kMaxWidth);
        s.outline.rgb &= 0xffffff;
        s.shadow.rgb &= 0xffffff;
        s.shadow.opacity = std::clamp(s.shadow.opacity, 0.0f, 1.0f);
        s.shadow.distance = std::clamp(s.shadow.distance, 0.0f, Shadow::kMaxDistance);
        s.shadow.blur = std::clamp(s.shadow.blur, 0.0f, Shadow::kMaxBlur);
        s.box.rgb &= 0xffffff;
        s.box.opacity = std::clamp(s.box.opacity, 0.0f, 1.0f);
        s.box.padding = std::clamp(s.box.padding, 0.0f, Box::kMaxPadding);
        s.box.cornerRadius = std::clamp(s.box.cornerRadius, 0.0f, 0.5f);
        return s;
    }
    bool operator==(const Style &) const = default;
};

// Where a title's picture is drawn, from its look (centreX = the anchor the
// alignment names, width = the text size): the picture is `widthPerSize`
// times the text size wide (in output widths), and its anchor sits
// `anchorX` of the way across it (titlerender::Picture). Playback, the
// export and dragging in the view all place a title through this.
inline inset::Look drawnLook(inset::Look l, double widthPerSize, double anchorX)
{
    return inset::anchored(l, widthPerSize, anchorX);
}

// A title has no source file, but trims and keyframes work in source time
// like a video's: it gets a long made-up source and starts in its middle, so
// either edge can be dragged out.
constexpr qint64 kSourceMs = 4LL * 3600 * 1000;
constexpr qint64 kDefaultSpanMs = 5000;
// The plain title's look when added: centred, text 8% of the frame high.
constexpr float kDefaultSize = 0.08f;
inline inset::Look defaultLook()
{
    inset::Look l;
    l.centerX = 0.5f;
    l.centerY = 0.5f;
    l.width = kDefaultSize;
    return l;
}

// A title dragged from the Overlays tab onto the timeline.
constexpr const char *kMimeType = "application/x-orbis-reframe-title";

} // namespace title
