#pragma once

#include "SourceTimeMap.h"
#include "Title.h"

#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QString>

#include <vector>

// A title's clock: fields in its text that show when the picture under it
// was recorded. `{time}` is the time of day in hours and minutes, `{time:ss}`
// with seconds, `{date}` the date written in full; `{{` is a brace of its
// own. The text box shows the fields, the picture their values (withFields)
// at each moment, from the main-track clip under the title then
// (title::Clock).
namespace title {

// Does `text` have a field in it?
bool hasFields(const QString &text);

// `text` with its fields filled in for the moment `at` (a wall-clock time,
// read as written: its date() and time()), written as `locale` writes a
// time and a date -- the hours as `hours` says; an invalid `at` (nothing
// says when it was recorded) gives `--:--`, `--:--:--` and an empty date.
// `{{` becomes `{`; a brace that isn't a field stays as it is.
QString withFields(const QString &text, const QDateTime &at, const QLocale &locale = QLocale(),
                   Hours hours = Hours::System);
// Does `text` have a field that shows the time (so Hours matters)?
bool hasTimeField(const QString &text);

// The text of an older project's title (before fields): every brace
// escaped, so it reads as the text it was.
QString escapeBraces(const QString &text);

// What the Insert menu puts in.
inline const QString kTimeField = QStringLiteral("{time}");
inline const QString kTimeSecondsField = QStringLiteral("{time:ss}");
inline const QString kDateField = QStringLiteral("{date}");

// When the picture at each moment of the timeline was recorded: the file
// playing there (SourceTimeMap) gives its recording start plus how far into
// it the picture is. A copy, safe to use on any thread.
class Clock {
public:
    Clock() = default;
    // `recorded`: each file's recording start by its path; a file that
    // isn't there, or an invalid time, is a clip nothing says the time of.
    Clock(SourceTimeMap map, const QHash<QString, QDateTime> &recorded);

    bool isEmpty() const { return m_map.isEmpty(); }
    // The wall-clock time at `timelineMs` in whole seconds (the camera
    // writes its recording start to the second; the first frame's fraction
    // of a second isn't known), invalid where nothing says.
    QDateTime at(qint64 timelineMs) const;
    // The first timeline ms after `timelineMs` where at() lands in another
    // `stepSeconds`-long stretch of the wall clock (a new second or minute)
    // or another clip starts, or -1 if none does.
    qint64 nextTick(qint64 timelineMs, int stepSeconds) const;

private:
    // Span `i`'s recording start, ms since the epoch of a clock read as UTC
    // (no daylight-saving jumps).
    qint64 baseMs(int i) const;

    SourceTimeMap m_map;
    std::vector<QDateTime> m_recorded; // per span
};

} // namespace title
