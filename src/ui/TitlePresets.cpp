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

} // namespace titlepresets
