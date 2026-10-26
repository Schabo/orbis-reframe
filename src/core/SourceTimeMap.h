#pragma once

#include <QString>
#include <QtGlobal>

#include <utility>
#include <vector>

// Which file plays at each moment of the timeline, and how far into it: the
// main-track clip there (at a cut the later one, as Timeline::timelineToClip)
// and its source time, so trims, speed changes and cuts carry through. What
// a title's clock (title::Clock) and the GPS telemetry (Telemetry) read. A
// copy of the timeline's numbers, safe to use on any thread.
class SourceTimeMap {
public:
    struct Span {
        qint64 startMs = 0;  // timeline
        qint64 endMs = 0;
        QString path;        // the clip's file
        qint64 trimInMs = 0; // source time at startMs
        double speed = 1.0;
    };

    SourceTimeMap() = default;
    // `spans` in timeline order, back to back from 0.
    explicit SourceTimeMap(std::vector<Span> spans) : m_spans(std::move(spans)) {}

    bool isEmpty() const { return m_spans.empty(); }
    const std::vector<Span> &spans() const { return m_spans; }

    // The span at `timelineMs` -- past the end the last one -- or -1 when
    // there is none.
    int spanAt(qint64 timelineMs) const;
    // Span `i`'s source time at `timelineMs`, clamped to the span.
    qint64 sourceMs(int i, qint64 timelineMs) const;
    // The first timeline ms from `fromMs` on in span `i` where its source
    // time reaches `sourceMs`, or -1 if it doesn't before the span ends.
    qint64 timelineMsAt(int i, qint64 fromMs, qint64 sourceMs) const;

private:
    std::vector<Span> m_spans;
};
