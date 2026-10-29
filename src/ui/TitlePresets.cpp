#include "TitlePresets.h"

#include "../core/TitleClock.h"

#include <QCoreApplication>
#include <QFontDatabase>

namespace titlepresets {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("TitlePresets", text);
}

inset::Look at(float x, float y, float size)
{
    inset::Look l;
    l.centerX = x;
    l.centerY = y;
    l.width = size;
    return l;
}

title::Shadow softShadow(float opacity, float distance, float blur)
{
    title::Shadow s;
    s.on = true;
    s.opacity = opacity;
    s.distance = distance;
    s.blur = blur;
    return s;
}

inset::Motion::Part part(inset::Motion::Kind kind, int64_t ms,
                         inset::Motion::Side side = inset::Motion::Side::Left)
{
    return {kind, side, ms};
}

title::Box box(uint32_t rgb, float opacity, float padding, float cornerRadius)
{
    title::Box b;
    b.on = true;
    b.rgb = rgb;
    b.opacity = opacity;
    b.padding = padding;
    b.cornerRadius = cornerRadius;
    return b;
}

QList<Preset> make()
{
    QList<Preset> list;
    const QString hint = tr("Drag it onto the timeline, or double-click to add it at the "
                            "playhead; edit it in the Title tab on the right.");

    // As the plain title has always been: white, bold, in the middle.
    Preset text{QStringLiteral("plain"), tr("Text"), tr("Plain text.") + QLatin1Char(' ') + hint};
    text.style.text = tr("Title");
    text.look = at(0.5f, 0.5f, title::kDefaultSize);
    list.push_back(text);

    Preset heading{QStringLiteral("heading"), tr("Heading"),
                   tr("A big title in the middle, with a soft shadow to stand out from the "
                      "picture.") +
                       QLatin1Char(' ') + hint};
    heading.style.text = tr("Title");
    heading.style.letterSpacing = 0.02f;
    heading.style.shadow = softShadow(0.6f, 0.04f, 0.1f);
    heading.look = at(0.5f, 0.45f, 0.12f);
    list.push_back(heading);

    Preset outlined{QStringLiteral("heading-outlined"), tr("Heading, outlined"),
                    tr("A big title in spaced-out capitals with a black outline, readable over "
                       "a bright sky or snow.") +
                        QLatin1Char(' ') + hint};
    outlined.style.text = tr("TITLE");
    outlined.style.letterSpacing = 0.12f;
    outlined.style.outline.on = true;
    outlined.style.outline.width = 0.06f;
    outlined.look = at(0.5f, 0.45f, 0.11f);
    list.push_back(outlined);

    // A heading that pops up and away.
    Preset pop{QStringLiteral("heading-pop"), tr("Heading, pop"),
               tr("A big title in the middle that pops up at the start and shrinks away at "
                  "the end.") +
                   QLatin1Char(' ') + hint};
    pop.style = heading.style;
    pop.look = heading.look;
    pop.motion.in = part(inset::Motion::Kind::Pop, 400);
    pop.motion.out = part(inset::Motion::Kind::Pop, 300);
    list.push_back(pop);

    // Lower thirds: left-aligned, so the text grows to the right from where
    // it starts.
    Preset lower{QStringLiteral("lower-third"), tr("Lower third"),
                 tr("Two lines in the lower left on a dark box: who or what, and a line about "
                    "it.") +
                     QLatin1Char(' ') + hint};
    lower.style.text = tr("Name\nA line about it");
    lower.style.align = title::Align::Left;
    lower.style.box = box(0x000000, 0.6f, 0.4f, 0.0f);
    lower.look = at(0.06f, 0.8f, 0.045f);
    list.push_back(lower);

    Preset bar{QStringLiteral("lower-third-bar"), tr("Lower third, bar"),
               tr("One line in the lower left on a solid coloured bar.") + QLatin1Char(' ') +
                   hint};
    bar.style.text = tr("Name");
    bar.style.align = title::Align::Left;
    bar.style.letterSpacing = 0.03f;
    bar.style.box = box(0xc8441c, 1.0f, 0.35f, 0.0f);
    bar.look = at(0.06f, 0.82f, 0.05f);
    list.push_back(bar);

    // A lower third that slides in from the left and back out.
    Preset sliding{QStringLiteral("lower-third-slide"), tr("Lower third, sliding"),
                   tr("A lower third on a dark box that slides in from the left and back out "
                      "at the end.") +
                       QLatin1Char(' ') + hint};
    sliding.style = lower.style;
    sliding.look = lower.look;
    sliding.motion.in = part(inset::Motion::Kind::Slide, 600);
    sliding.motion.out = part(inset::Motion::Kind::Slide, 600);
    list.push_back(sliding);

    Preset caption{QStringLiteral("caption"), tr("Caption"),
                   tr("Small text at the bottom in the middle, on a rounded box.") +
                       QLatin1Char(' ') + hint};
    caption.style.text = tr("Caption");
    caption.style.bold = false;
    caption.style.box = box(0x000000, 0.5f, 0.35f, 0.5f);
    caption.look = at(0.5f, 0.88f, 0.04f);
    list.push_back(caption);

    // A caption typed on letter by letter, and off again.
    Preset typed{QStringLiteral("caption-typed"), tr("Caption, typed"),
                 tr("A caption that types itself on at the start and off at the end.") +
                     QLatin1Char(' ') + hint};
    typed.style = caption.style;
    typed.look = caption.look;
    typed.motion.in = part(inset::Motion::Kind::Type, 1500);
    typed.motion.out = part(inset::Motion::Kind::Type, 600);
    list.push_back(typed);

    // Dates go where they stay out of the way: a corner.
    // The date and the clock are fields (title::withFields): they show when
    // the clip under the title was recorded, following it across cuts.
    const QString dateHint = tr("The date is the recording date of the clip under it, where "
                                "its file says.");
    Preset date{QStringLiteral("date"), tr("Date"),
                tr("The date in the lower right.") + QLatin1Char(' ') + dateHint +
                    QLatin1Char(' ') + hint};
    date.style.text = title::kDateField;
    date.style.bold = false;
    date.style.align = title::Align::Right;
    date.style.shadow = softShadow(0.7f, 0.05f, 0.06f);
    date.look = at(0.95f, 0.92f, 0.04f);
    list.push_back(date);

    Preset clock{QStringLiteral("clock"), tr("Clock"),
                 tr("The time of day in the lower right, running as the clip plays: when the "
                    "clip under it was recorded, where its file says.") +
                     QLatin1Char(' ') + hint};
    clock.style.text = title::kTimeField;
    clock.style.bold = false;
    clock.style.align = title::Align::Right;
    clock.style.shadow = softShadow(0.7f, 0.05f, 0.06f);
    clock.look = at(0.95f, 0.92f, 0.04f);
    list.push_back(clock);

    Preset place{QStringLiteral("date-place"), tr("Date and place"),
                 tr("The date and, below it, a place to type, in the lower left.") +
                     QLatin1Char(' ') + dateHint + QLatin1Char(' ') + hint};
    place.style.text = title::kDateField + QLatin1Char('\n') + tr("Place");
    place.style.bold = false;
    place.style.align = title::Align::Left;
    place.style.lineSpacing = 1.1f;
    place.style.shadow = softShadow(0.7f, 0.05f, 0.06f);
    place.look = at(0.05f, 0.88f, 0.04f);
    list.push_back(place);

    // The GPS fields (title::withFields): where the camera was, from the
    // clip under the title, as it plays. Bottom left, out of the clock's
    // corner.
    const QString gpsHint = tr("The values come from the GPS track of the clip under it, where "
                               "the camera recorded one; without one they show --.");
    Preset speed{QStringLiteral("speed"), tr("Speed"),
                 tr("The speed in the lower left, running as the clip plays.") +
                     QLatin1Char(' ') + gpsHint + QLatin1Char(' ') + hint};
    speed.style.text = title::kSpeedField;
    speed.style.align = title::Align::Left;
    speed.style.shadow = softShadow(0.7f, 0.05f, 0.06f);
    speed.look = at(0.05f, 0.9f, 0.06f);
    list.push_back(speed);

    Preset speedAltitude{QStringLiteral("speed-altitude"), tr("Speed and altitude"),
                         tr("The speed and, below it, the altitude, in the lower left.") +
                             QLatin1Char(' ') + gpsHint + QLatin1Char(' ') + hint};
    speedAltitude.style.text = title::kSpeedField + QLatin1Char('\n') + title::kAltitudeField;
    speedAltitude.style.align = title::Align::Left;
    speedAltitude.style.lineSpacing = 1.1f;
    speedAltitude.style.shadow = softShadow(0.7f, 0.05f, 0.06f);
    speedAltitude.look = at(0.05f, 0.87f, 0.05f);
    list.push_back(speedAltitude);

    Preset direction{QStringLiteral("direction"), tr("Direction"),
                     tr("The direction of travel at the top in the middle, as a compass point "
                        "and in degrees.") +
                         QLatin1Char(' ') + gpsHint + QLatin1Char(' ') + hint};
    direction.style.text = title::kCompassField + QStringLiteral("  ") + title::kHeadingField;
    direction.style.bold = false;
    direction.style.box = box(0x000000, 0.5f, 0.35f, 0.5f);
    direction.look = at(0.5f, 0.07f, 0.04f);
    list.push_back(direction);

    return list;
}

} // namespace

const QList<Preset> &all()
{
    static const QList<Preset> presets = make();
    return presets;
}

const Preset *find(const QString &id)
{
    for (const Preset &p : all())
        if (p.id == id)
            return &p;
    return nullptr;
}

title::Style styleFor(const Preset &p)
{
    title::Style s = p.style;
    s.font = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    s.units = title::defaultUnits();
    return s;
}

namespace {

QList<GaugePreset> makeGauges()
{
    QList<GaugePreset> list;
    const QString hint = tr("Drag it onto the timeline, or double-click to add it at the "
                            "playhead; edit it in the Title tab on the right.");
    const QString gpsHint = tr("It shows the GPS track of the clip under it, where the camera "
                               "recorded one; without one it stays empty.");
    const auto at = [](float x, float y, float size) {
        inset::Look l;
        l.centerX = x;
        l.centerY = y;
        l.width = size;
        return l;
    };
    GaugePreset dial{QStringLiteral("gauge-speed-dial"), tr("Speedometer"),
                     tr("A round speedometer in the lower right: a needle over the scale, the "
                        "speed in the middle.") +
                         QLatin1Char(' ') + gpsHint + QLatin1Char(' ') + hint};
    dial.look = at(0.88f, 0.75f, 0.28f);
    list.push_back(dial);

    GaugePreset speedBar{QStringLiteral("gauge-speed-bar"), tr("Speed bar"),
                         tr("The speed in the lower left, over a bar that fills up with it.") +
                             QLatin1Char(' ') + gpsHint + QLatin1Char(' ') + hint};
    speedBar.style.kind = gauge::Kind::Bar;
    speedBar.look = at(0.15f, 0.88f, 0.13f);
    list.push_back(speedBar);

    GaugePreset altitudeBar{QStringLiteral("gauge-altitude-bar"), tr("Altitude bar"),
                            tr("The altitude in the lower left, over a bar that fills up with "
                               "it.") +
                                QLatin1Char(' ') + gpsHint + QLatin1Char(' ') + hint};
    altitudeBar.style.kind = gauge::Kind::Bar;
    altitudeBar.style.reads = gauge::Reads::Altitude;
    altitudeBar.look = at(0.15f, 0.88f, 0.13f);
    list.push_back(altitudeBar);

    GaugePreset tape{QStringLiteral("gauge-heading-tape"), tr("Heading tape"),
                     tr("The direction of travel at the top in the middle: a compass strip "
                        "sliding under a marker, the degrees above it.") +
                         QLatin1Char(' ') + gpsHint + QLatin1Char(' ') + hint};
    tape.style.kind = gauge::Kind::Tape;
    tape.look = at(0.5f, 0.1f, 0.12f);
    list.push_back(tape);

    // The motion gauges read the camera's motion sensor, GPS or not.
    const QString motionHint = tr("It shows the motion the camera's sensors recorded in the "
                                  "clip under it, about the direction it travels.");
    GaugePreset lean{QStringLiteral("gauge-lean"), tr("Lean angle"),
                     tr("How far the vehicle leans, at the bottom in the middle: a half dial, "
                        "upright at the top, the degrees and the side in the middle.") +
                         QLatin1Char(' ') + motionHint + QLatin1Char(' ') + hint};
    lean.style.kind = gauge::Kind::Lean;
    lean.style.min = -60.0; // its scale either side of upright
    lean.style.max = 60.0;
    lean.look = at(0.5f, 0.86f, 0.18f);
    list.push_back(lean);

    GaugePreset g{QStringLiteral("gauge-g-force"), tr("G-force"),
                  tr("The acceleration in the lower left: a dot in a circle, to the side in a "
                     "bend, up speeding up and down braking, the total in g under it. Where a "
                     "clip hardly turns the direction of travel isn't known, and it stays "
                     "empty.") +
                      QLatin1Char(' ') + motionHint + QLatin1Char(' ') + hint};
    g.style.kind = gauge::Kind::GForce;
    g.style.min = -1.0;
    g.style.max = 1.0;
    g.look = at(0.12f, 0.75f, 0.25f);
    list.push_back(g);

    GaugePreset route{QStringLiteral("gauge-route"), tr("Route"),
                      tr("The whole trip seen from above in the upper right, north up: the "
                         "route of every clip on the timeline, the part ridden so far "
                         "coloured, a dot where the camera is; clips with no GPS track are "
                         "left out. It shows where you rode: think before sharing it.") +
                          QLatin1Char(' ') + hint};
    route.style.kind = gauge::Kind::Route;
    route.look = at(0.87f, 0.22f, 0.3f);
    list.push_back(route);
    return list;
}

} // namespace

const QList<GaugePreset> &gauges()
{
    static const QList<GaugePreset> presets = makeGauges();
    return presets;
}

const GaugePreset *findGauge(const QString &id)
{
    for (const GaugePreset &p : gauges())
        if (p.id == id)
            return &p;
    return nullptr;
}

gauge::Style gaugeStyleFor(const GaugePreset &p)
{
    gauge::Style s = p.style;
    s.font = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    s.units = title::defaultUnits();
    const gauge::Range r = gauge::defaultRange(s.kind, s.reads, s.units);
    s.min = r.min;
    s.max = r.max;
    return s;
}

} // namespace titlepresets
