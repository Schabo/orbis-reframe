#include "GaugeSource.h"

#include <algorithm>
#include <utility>

GaugeSource::GaugeSource(gauge::Style style, int64_t spanMs, int heightPx, int outW, int outH,
                         title::Clock clock, int64_t startMs)
    : m_style(std::move(style)), m_spanMs(spanMs), m_heightPx(heightPx),
      m_widthPerSize(outW > 0 ? gauge::aspect(m_style.kind) * outH / outW : 0.0),
      m_clock(std::move(clock)), m_startMs(startMs),
      m_route(m_style.kind == gauge::Kind::Route ? gaugerender::routeOf(m_clock) : nullptr)
{
}

gaugerender::Reading GaugeSource::readingAt(int64_t localMs) const
{
    const int64_t t = m_startMs + localMs;
    gaugerender::Moment m;
    m.timelineMs = t;
    if (gauge::readsMotion(m_style.kind))
        m.motion = m_clock.motionAt(t);
    else
        m.gps = m_clock.gpsAt(t);
    return gaugerender::readingOf(m_style, m, m_heightPx, m_route.get());
}

exportpipe::Still GaugeSource::still(const gaugerender::Picture &pic) const
{
    exportpipe::Still s;
    if (pic.empty())
        return s;
    s.rgba = pic.rgba;
    s.width = pic.width;
    s.height = pic.height;
    s.widthPerSize = m_widthPerSize;
    s.anchorX = 0.5;
    return s;
}

exportpipe::Still GaugeSource::at(int64_t localMs)
{
    return still(gaugerender::render(m_style, m_heightPx, readingAt(localMs), m_route.get()));
}

exportpipe::Still GaugeSource::ready(int64_t localMs)
{
    return still(gaugerender::cached(m_style, m_heightPx, readingAt(localMs), m_route.get()));
}

int64_t GaugeSource::nextChange(int64_t localMs)
{
    // A reading runs smoothly, with no tick to jump to: looked for a step
    // at a time, only a little ahead -- playback asks again as it goes, and
    // has the change ready before it shows.
    const gaugerender::Reading now = readingAt(localMs);
    const int64_t end = std::min(m_spanMs, localMs + kLookaheadMs);
    for (int64_t t = localMs + kStepMs; t <= end; t += kStepMs)
        if (readingAt(t) != now) {
            int64_t lo = t - kStepMs, hi = t; // same at lo, differs at hi
            while (hi - lo > 1) {
                const int64_t mid = lo + (hi - lo) / 2;
                (readingAt(mid) == now ? lo : hi) = mid;
            }
            return hi;
        }
    return -1;
}
