#include "TitlePresets.h"

#include <QCoreApplication>
#include <QFontDatabase>
#include <QLocale>

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

    Preset caption{QStringLiteral("caption"), tr("Caption"),
                   tr("Small text at the bottom in the middle, on a rounded box.") +
                       QLatin1Char(' ') + hint};
    caption.style.text = tr("Caption");
    caption.style.bold = false;
    caption.style.box = box(0x000000, 0.5f, 0.35f, 0.5f);
    caption.look = at(0.5f, 0.88f, 0.04f);
    list.push_back(caption);

    // Dates go where they stay out of the way: a corner.
    const QString dateHint = tr("The date is the clip's recording date, where its file says.");
    Preset date{QStringLiteral("date"), tr("Date"),
                tr("The date in the lower right.") + QLatin1Char(' ') + dateHint +
                    QLatin1Char(' ') + hint};
    date.style.text = tr("Date");
    date.style.bold = false;
    date.style.align = title::Align::Right;
    date.style.shadow = softShadow(0.7f, 0.05f, 0.06f);
    date.look = at(0.95f, 0.92f, 0.04f);
    date.date = true;
    list.push_back(date);

    Preset place{QStringLiteral("date-place"), tr("Date and place"),
                 tr("The date and, below it, a place to type, in the lower left.") +
                     QLatin1Char(' ') + dateHint + QLatin1Char(' ') + hint};
    place.style.text = tr("Date") + QLatin1Char('\n') + tr("Place");
    place.style.bold = false;
    place.style.align = title::Align::Left;
    place.style.lineSpacing = 1.1f;
    place.style.shadow = softShadow(0.7f, 0.05f, 0.06f);
    place.look = at(0.05f, 0.88f, 0.04f);
    place.date = true;
    list.push_back(place);

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

title::Style styleFor(const Preset &p, const QDateTime &recorded)
{
    title::Style s = p.style;
    s.font = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    if (p.date && recorded.isValid()) {
        const qsizetype newline = s.text.indexOf(QLatin1Char('\n'));
        const QString rest = newline < 0 ? QString() : s.text.mid(newline);
        s.text = QLocale().toString(recorded.date(), QLocale::LongFormat) + rest;
    }
    return s;
}

} // namespace titlepresets
