#include "TitleClock.h"

#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace title {

namespace {

enum class Field {
    None, Time, TimeSeconds, Date, Speed, Altitude, Heading, Compass, Lean, G
};

// The field starting at `i` (a '{'), and how long it is.
Field fieldAt(const QString &text, qsizetype i, qsizetype *length)
{
    for (const auto &[name, field] : {std::pair{&kTimeField, Field::Time},
                                      std::pair{&kTimeSecondsField, Field::TimeSeconds},
                                      std::pair{&kDateField, Field::Date},
                                      std::pair{&kSpeedField, Field::Speed},
                                      std::pair{&kAltitudeField, Field::Altitude},
                                      std::pair{&kHeadingField, Field::Heading},
                                      std::pair{&kCompassField, Field::Compass},
                                      std::pair{&kLeanField, Field::Lean},
                                      std::pair{&kGField, Field::G}})
        if (QStringView(text).mid(i).startsWith(*name)) {
            *length = name->size();
            return field;
        }
    return Field::None;
}

bool isTelemetry(Field f)
{
    return f == Field::Speed || f == Field::Altitude || f == Field::Heading ||
           f == Field::Compass || f == Field::Lean || f == Field::G;
}

// Is there a field in `text` that `wanted` picks?
template <typename Pred>
bool anyField(const QString &text, Pred wanted)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) != QLatin1Char('{'))
            continue;
        if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('{')) {
            ++i; // an escaped brace
            continue;
        }
        qsizetype length = 0;
        const Field f = fieldAt(text, i, &length);
        if (f != Field::None && wanted(f))
            return true;
    }
    return false;
}

QString gpsValue(Field f, const std::optional<Telemetry::Values> &gps, Units units,
                 const QLocale &locale)
{
    const QString none = QStringLiteral("--");
    switch (f) {
    case Field::Speed: {
        const QString unit = QLatin1Char(' ') + QLatin1String(unitName(units.speed));
        if (!gps)
            return none + unit;
        return locale.toString(qRound(speedIn(gps->speedMps, units.speed))) + unit;
    }
    case Field::Altitude: {
        const QString unit = QLatin1Char(' ') + QLatin1String(unitName(units.altitude));
        if (!gps)
            return none + unit;
        return locale.toString(qRound(altitudeIn(gps->altitudeM, units.altitude))) + unit;
    }
    case Field::Heading:
        if (!gps || !gps->headingDeg)
            return none + QChar(0x00b0);
        return locale.toString(qRound(*gps->headingDeg) % 360) + QChar(0x00b0);
    case Field::Compass: {
        if (!gps || !gps->headingDeg)
            return none;
        static const char *const points[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        return QLatin1String(points[qRound(*gps->headingDeg / 45.0) % 8]);
    }
    default: return {};
    }
}

// A time format (QLocale's) with its hours as `hours` says: 24-hour drops
// the AM / PM and counts the hours to 23 ("HH"), 12-hour counts them to 12
// ("h") and adds AM / PM after the time if the format has none. Quoted text
// is kept as it is.
QString withHours(const QString &format, Hours hours)
{
    if (hours == Hours::System)
        return format;
    const auto isAmPm = [](QChar c) { return c == QLatin1Char('a') || c == QLatin1Char('A'); };
    bool amPm = false;
    for (qsizetype i = 0, quoted = 0; i < format.size(); ++i) {
        if (format.at(i) == QLatin1Char('\''))
            quoted ^= 1;
        else if (!quoted && isAmPm(format.at(i)))
            amPm = true;
    }
    if (hours == Hours::H12 && amPm)
        return format;
    QString out;
    bool dropSpace = false; // after a dropped AM / PM
    for (qsizetype i = 0; i < format.size(); ++i) {
        const QChar c = format.at(i);
        if (c == QLatin1Char('\'')) { // a quoted literal, to its end
            const qsizetype end = format.indexOf(QLatin1Char('\''), i + 1);
            const qsizetype stop = end < 0 ? format.size() : end + 1;
            out += format.mid(i, stop - i);
            i = stop - 1;
            dropSpace = false;
            continue;
        }
        if (c == QLatin1Char('h') || c == QLatin1Char('H')) {
            while (i + 1 < format.size() && format.at(i + 1) == c)
                ++i;
            out += hours == Hours::H24 ? QStringLiteral("HH") : QStringLiteral("h");
            dropSpace = false;
            continue;
        }
        if (isAmPm(c)) { // only with H24 here: "a", "A", "ap", "AP", ...
            if (i + 1 < format.size() && format.at(i + 1).toLower() == QLatin1Char('p'))
                ++i;
            while (!out.isEmpty() && out.back().isSpace())
                out.chop(1);
            dropSpace = true;
            continue;
        }
        if (dropSpace && c.isSpace())
            continue;
        dropSpace = false;
        out += c;
    }
    if (hours == Hours::H12)
        out += QStringLiteral(" Ap"); // as the locale writes AM / PM
    return out.trimmed();
}

// The locale's short time with seconds after the minutes, separated as it
// separates the minutes from the hours. (Its long format adds the time
// zone, which a camera's clock doesn't know.)
QString secondsFormat(QString format)
{
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
    return anyField(text, [](Field) { return true; });
}

bool hasClockFields(const QString &text)
{
    return anyField(text, [](Field f) { return !isTelemetry(f); });
}

bool hasTelemetryFields(const QString &text)
{
    return anyField(text, isTelemetry);
}

bool hasGpsFields(const QString &text)
{
    return anyField(text,
                    [](Field f) { return isTelemetry(f) && f != Field::Lean && f != Field::G; });
}

bool hasMotionFields(const QString &text)
{
    return anyField(text, [](Field f) { return f == Field::Lean || f == Field::G; });
}

bool hasGForceField(const QString &text)
{
    return anyField(text, [](Field f) { return f == Field::G; });
}

int leanDegrees(double leanDeg)
{
    return int(std::lround(leanDeg));
}

QString leanSide(int degrees)
{
    return degrees > 0 ? QStringLiteral("R") : degrees < 0 ? QStringLiteral("L") : QString();
}

QString leanText(const std::optional<MotionTelemetry::Values> &motion, const QLocale &locale)
{
    if (!motion)
        return QStringLiteral("--") + QChar(0x00b0);
    const int d = leanDegrees(motion->leanDeg);
    const QString side = leanSide(d);
    return locale.toString(std::abs(d)) + QChar(0x00b0) +
           (side.isEmpty() ? QString() : QLatin1Char(' ') + side);
}

std::optional<int> gHundredths(const std::optional<MotionTelemetry::Values> &motion)
{
    if (!motion || !motion->travelKnown)
        return std::nullopt;
    return int(std::lround(100.0 * std::hypot(motion->lateralG, motion->forwardG)));
}

QString gNumber(int hundredths, const QLocale &locale)
{
    return locale.toString(hundredths / 100.0, 'f', 2);
}

QString gText(const std::optional<MotionTelemetry::Values> &motion, const QLocale &locale)
{
    const std::optional<int> g = gHundredths(motion);
    return (g ? gNumber(*g, locale) : QStringLiteral("--")) + QStringLiteral(" g");
}

Units defaultUnits(const QLocale &locale)
{
    if (locale.measurementSystem() == QLocale::MetricSystem)
        return {};
    return {SpeedUnit::Mph, AltitudeUnit::Feet};
}

QString withFields(const QString &text, const QDateTime &at, const QLocale &locale,
                   Hours hours, const std::optional<Telemetry::Values> &gps, Units units,
                   const std::optional<MotionTelemetry::Values> &motion)
{
    if (!text.contains(QLatin1Char('{')))
        return text;
    const QString timeFormat = withHours(locale.timeFormat(QLocale::ShortFormat), hours);
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
        const Field f = fieldAt(text, i, &length);
        switch (f) {
        case Field::None: out += ch; continue;
        case Field::Time:
            out += at.isValid() ? locale.toString(at.time(), timeFormat)
                                : QStringLiteral("--:--");
            break;
        case Field::TimeSeconds:
            out += at.isValid() ? locale.toString(at.time(), secondsFormat(timeFormat))
                                : QStringLiteral("--:--:--");
            break;
        case Field::Date:
            if (at.isValid())
                out += locale.toString(at.date(), QLocale::LongFormat);
            break;
        case Field::Speed:
        case Field::Altitude:
        case Field::Heading:
        case Field::Compass: out += gpsValue(f, gps, units, locale); break;
        case Field::Lean: out += leanText(motion, locale); break;
        case Field::G: out += gText(motion, locale); break;
        }
        i += length - 1;
    }
    return out;
}

bool hasTimeField(const QString &text)
{
    return anyField(text, [](Field f) { return f == Field::Time || f == Field::TimeSeconds; });
}

QString escapeBraces(const QString &text)
{
    QString out = text;
    return out.replace(QLatin1Char('{'), QStringLiteral("{{"));
}

Clock::Clock(SourceTimeMap map, const QHash<QString, QDateTime> &recorded, const Telemetries &gps,
             const Motions &motion)
    : m_map(std::move(map))
{
    for (const SourceTimeMap::Span &s : m_map.spans()) {
        m_recorded.push_back(recorded.value(s.path));
        m_gps.push_back(gps.value(s.path));
        m_motion.push_back(motion.value(s.path));
    }
}

std::optional<Telemetry::Values> Clock::gpsAt(qint64 timelineMs) const
{
    const int i = m_map.spanAt(timelineMs);
    if (i < 0 || !m_gps[size_t(i)])
        return std::nullopt;
    return m_gps[size_t(i)]->at(double(m_map.sourceMs(i, timelineMs)));
}

std::optional<MotionTelemetry::Values> Clock::motionAt(qint64 timelineMs) const
{
    const int i = m_map.spanAt(timelineMs);
    if (i < 0 || !m_motion[size_t(i)])
        return std::nullopt;
    return m_motion[size_t(i)]->at(double(m_map.sourceMs(i, timelineMs)));
}

std::vector<std::vector<Clock::Fix>> Clock::route(qint64 stepMs) const
{
    std::vector<std::vector<Fix>> out;
    stepMs = std::max<qint64>(1, stepMs);
    for (size_t i = 0; i < m_map.spans().size(); ++i) {
        const SourceTimeMap::Span &s = m_map.spans()[i];
        if (!m_gps[i])
            continue;
        bool open = false; // the last stretch still runs
        for (qint64 t = s.startMs;; t += stepMs) {
            const qint64 at = std::min(t, s.endMs - 1); // the clip's last ms
            const std::optional<Telemetry::Values> v =
                m_gps[i]->at(double(m_map.sourceMs(int(i), at)));
            if (!v)
                open = false;
            else {
                if (!open)
                    out.emplace_back();
                out.back().push_back({at, v->latitude, v->longitude});
                open = true;
            }
            if (at >= s.endMs - 1)
                break;
        }
    }
    return out;
}

qint64 Clock::baseMs(int i) const
{
    const QDateTime &r = m_recorded[size_t(i)];
    return QDateTime(r.date(), r.time(), QTimeZone::UTC).toMSecsSinceEpoch();
}

QDateTime Clock::at(qint64 timelineMs) const
{
    const int i = m_map.spanAt(timelineMs);
    if (i < 0 || !m_recorded[size_t(i)].isValid())
        return {};
    const qint64 seconds = floorDiv(baseMs(i) + m_map.sourceMs(i, timelineMs), 1000);
    return QDateTime::fromMSecsSinceEpoch(seconds * 1000, QTimeZone::UTC);
}

qint64 Clock::nextTick(qint64 timelineMs, int stepSeconds) const
{
    const int i = m_map.spanAt(timelineMs);
    if (i < 0)
        return -1;
    const SourceTimeMap::Span &s = m_map.spans()[size_t(i)];
    if (m_recorded[size_t(i)].isValid() && stepSeconds > 0) {
        const qint64 step = qint64(stepSeconds) * 1000;
        const qint64 base = baseMs(i);
        const qint64 target = (floorDiv(base + m_map.sourceMs(i, timelineMs), step) + 1) * step;
        const qint64 t = m_map.timelineMsAt(i, timelineMs + 1, target - base);
        if (t >= 0)
            return t;
    }
    // The next clip's clock, if there is one after this.
    if (i + 1 < int(m_map.spans().size()) && s.endMs > timelineMs)
        return s.endMs;
    return -1;
}

} // namespace title
