#include "TitleSource.h"

#include "TitleRender.h"

#include <algorithm>
#include <cmath>
#include <utility>

TitleSource::TitleSource(title::Style style, inset::Motion motion, int64_t spanMs, int fontPx,
                         int outW, int outH, title::Clock clock, int64_t startMs)
    : m_style(std::move(style)), m_motion(motion), m_spanMs(spanMs), m_fontPx(fontPx),
      m_widthScale(outW > 0 ? double(outH) / outW : 0.0),
      m_types(motion.in.kind == inset::Motion::Kind::Type ||
              motion.out.kind == inset::Motion::Kind::Type),
      m_clock(std::move(clock)), m_startMs(startMs), m_fields(title::hasFields(m_style.text)),
      m_clockStepS(m_style.text.contains(title::kTimeSecondsField) ? 1 : 60)
{
}

QString TitleSource::textAt(int64_t localMs) const
{
    return title::withFields(m_style.text,
                             m_fields ? m_clock.at(m_startMs + localMs) : QDateTime(), QLocale(),
                             m_style.hours);
}

exportpipe::Still TitleSource::still(int64_t localMs, int visible, bool make)
{
    title::Style style = m_style;
    style.text = textAt(localMs);
    // A clock's digits all as wide: it doesn't shift as it ticks.
    const titlerender::Picture pic =
        make ? titlerender::render(style, m_fontPx, visible, m_fields)
             : titlerender::cached(style, m_fontPx, visible, m_fields);
    exportpipe::Still s;
    if (pic.empty())
        return s;
    s.rgba = pic.rgba;
    s.width = pic.width;
    s.height = pic.height;
    s.widthPerSize = pic.widthPerFontPx * m_widthScale;
    s.anchorX = pic.anchorX;
    return s;
}

exportpipe::Still TitleSource::full()
{
    return still(0, -1, true);
}

int TitleSource::stateAt(int64_t localMs) const
{
    return m_types ? titlerender::visibleAt(titlerender::graphemeCount(textAt(localMs)), m_motion,
                                            localMs, m_spanMs)
                   : -1;
}

exportpipe::Still TitleSource::at(int64_t localMs)
{
    return still(localMs, stateAt(localMs), true);
}

exportpipe::Still TitleSource::ready(int64_t localMs)
{
    return still(localMs, stateAt(localMs), false);
}

int64_t TitleSource::nextTextChange(int64_t localMs) const
{
    if (!m_fields)
        return -1;
    // The clock ticks each second or minute, but the text needn't change
    // with it (a date at a new minute).
    const QString now = textAt(localMs);
    for (int64_t t = m_startMs + localMs;;) {
        t = m_clock.nextTick(t, m_clockStepS);
        if (t < 0 || t - m_startMs > m_spanMs)
            return -1;
        if (textAt(t - m_startMs) != now)
            return t - m_startMs;
    }
}

int64_t TitleSource::nextChange(int64_t localMs)
{
    const int64_t textChange = nextTextChange(localMs);
    if (!m_types)
        return textChange;
    // Until the text changes, the state only changes in the In and the Out,
    // and runs one way in each: in each stretch the first time it differs
    // is found by halving.
    const int64_t until = textChange >= 0 ? textChange - 1 : m_spanMs;
    const inset::MotionPhase phase = inset::motionPhase(m_motion, localMs, m_spanMs);
    const int64_t inEnd = static_cast<int64_t>(std::ceil(phase.inEndMs)) - 1;
    const int64_t outStart = static_cast<int64_t>(std::floor(phase.outStartMs));
    const int now = stateAt(localMs);
    int64_t from = localMs + 1;
    for (int64_t to : {inEnd, outStart, m_spanMs}) {
        to = std::min(to, until);
        if (from > to)
            continue;
        if (stateAt(from) != now)
            return from;
        if (stateAt(to) != now) {
            int64_t lo = from, hi = to; // same at lo, differs at hi
            while (hi - lo > 1) {
                const int64_t mid = lo + (hi - lo) / 2;
                (stateAt(mid) == now ? lo : hi) = mid;
            }
            return hi;
        }
        from = to + 1;
    }
    return textChange;
}
