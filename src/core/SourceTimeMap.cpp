#include "SourceTimeMap.h"

#include <algorithm>
#include <cmath>

int SourceTimeMap::spanAt(qint64 timelineMs) const
{
    if (m_spans.empty())
        return -1;
    for (size_t i = 0; i < m_spans.size(); ++i)
        if (timelineMs < m_spans[i].endMs)
            return int(i);
    return int(m_spans.size()) - 1;
}

qint64 SourceTimeMap::sourceMs(int i, qint64 timelineMs) const
{
    const Span &s = m_spans.at(size_t(i));
    const qint64 local = std::clamp(timelineMs, s.startMs, s.endMs) - s.startMs;
    return s.trimInMs + std::llround(local * s.speed);
}

qint64 SourceTimeMap::timelineMsAt(int i, qint64 fromMs, qint64 sourceMs) const
{
    const Span &s = m_spans.at(size_t(i));
    if (s.speed <= 0.0)
        return -1;
    // Close below the answer from the speed, then up to it (rounding).
    qint64 t = s.startMs + static_cast<qint64>(std::floor((sourceMs - s.trimInMs) / s.speed)) - 2;
    t = std::max(t, fromMs);
    while (t < s.endMs && this->sourceMs(i, t) < sourceMs)
        ++t;
    return t < s.endMs ? t : -1;
}
