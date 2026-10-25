#include "TitleClock.h"

#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace title {

namespace {

enum class Field { None, Time, TimeSeconds, Date };

// The field starting at `i` (a '{'), and how long it is.
Field fieldAt(const QString &text, qsizetype i, qsizetype *length)
{
    for (const auto &[name, field] : {std::pair{&kTimeField, Field::Time},
                                      std::pair{&kTimeSecondsField, Field::TimeSeconds},
                                      std::pair{&kDateField, Field::Date}})
        if (QStringView(text).mid(i).startsWith(*name)) {
            *length = name->size();
            return field;
        }
    return Field::None;
}

// The locale's short time with seconds after the minutes, separated as it
// separates the minutes from the hours. (Its long format adds the time
// zone, which a camera's clock doesn't know.)
QString secondsFormat(const QLocale &locale)
{
    QString format = locale.timeFormat(QLocale::ShortFormat);
    if (format.contains(QLatin1Char('s')))
        return format;
    const qsizetype mm = format.indexOf(QLatin1String("mm"));
    if (mm < 0)
        return format;
    const QChar before = mm > 0 ? format.at(mm - 1) : QLatin1Char(':');
    const QChar sep = before.isLetter() ? QLatin1Char(':') : before;
    return format.insert(mm + 2, sep + QStringLiteral("ss"));
}

qint64 floorDiv(qint64 a, qint64 b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

} // namespace

bool hasFields(const QString &text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) != QLatin1Char('{'))
            continue;
        if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('{')) {
            ++i; // an escaped brace
            continue;
        }
        qsizetype length = 0;
        if (fieldAt(text, i, &length) != Field::None)
            return true;
    }
    return false;
}

QString withFields(const QString &text, const QDateTime &at, const QLocale &locale)
{
    if (!text.contains(QLatin1Char('{')))
        return text;
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch != QLatin1Char('{')) {
            out += ch;
            continue;
        }
        if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('{')) {
            out += ch;
            ++i;
            continue;
        }
        qsizetype length = 0;
        switch (fieldAt(text, i, &length)) {
        case Field::None: out += ch; continue;
        case Field::Time:
            out += at.isValid() ? locale.toString(at.time(), QLocale::ShortFormat)
                                : QStringLiteral("--:--");
            break;
        case Field::TimeSeconds:
            out += at.isValid() ? locale.toString(at.time(), secondsFormat(locale))
                                : QStringLiteral("--:--:--");
            break;
        case Field::Date:
            if (at.isValid())
                out += locale.toString(at.date(), QLocale::LongFormat);
            break;
        }
        i += length - 1;
    }
    return out;
}

QString escapeBraces(const QString &text)
{
    QString out = text;
    return out.replace(QLatin1Char('{'), QStringLiteral("{{"));
}

const Clock::Span *Clock::spanAt(qint64 timelineMs) const
{
    if (m_spans.empty())
        return nullptr;
    for (const Span &s : m_spans)
        if (timelineMs < s.endMs)
            return &s;
    return &m_spans.back();
}

qint64 Clock::wallMs(const Span &s, qint64 timelineMs)
{
    const qint64 local = std::clamp(timelineMs, s.startMs, s.endMs) - s.startMs;
    const qint64 base =
        QDateTime(s.recorded.date(), s.recorded.time(), QTimeZone::UTC).toMSecsSinceEpoch();
    return base + s.trimInMs + std::llround(local * s.speed);
}

QDateTime Clock::at(qint64 timelineMs) const
{
    const Span *s = spanAt(timelineMs);
    if (!s || !s->recorded.isValid())
        return {};
    const qint64 seconds = floorDiv(wallMs(*s, timelineMs), 1000);
    return QDateTime::fromMSecsSinceEpoch(seconds * 1000, QTimeZone::UTC);
}

qint64 Clock::nextTick(qint64 timelineMs, int stepSeconds) const
{
    const Span *s = spanAt(timelineMs);
    if (!s)
        return -1;
    if (s->recorded.isValid() && s->speed > 0.0 && stepSeconds > 0) {
        const qint64 step = qint64(stepSeconds) * 1000;
        const qint64 target = (floorDiv(wallMs(*s, timelineMs), step) + 1) * step;
        // Close below the answer from the speed, then up to it (rounding).
        const qint64 base = wallMs(*s, s->startMs);
        qint64 t = s->startMs + static_cast<qint64>(std::floor((target - base) / s->speed)) - 2;
        t = std::max(t, timelineMs + 1);
        while (t < s->endMs && wallMs(*s, t) < target)
            ++t;
        if (t < s->endMs)
            return t;
    }
    // The next clip's clock, if there is one after this.
    if (s != &m_spans.back() && s->endMs > timelineMs)
        return s->endMs;
    return -1;
}

} // namespace title
