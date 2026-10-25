#include "TitleSource.h"

#include "TitleRender.h"

#include <algorithm>
#include <cmath>
#include <utility>

TitleSource::TitleSource(title::Style style, inset::Motion motion, int64_t spanMs, int fontPx,
                         int outW, int outH)
    : m_style(std::move(style)), m_motion(motion), m_spanMs(spanMs), m_fontPx(fontPx),
      m_widthScale(outW > 0 ? double(outH) / outW : 0.0),
      m_count(titlerender::graphemeCount(m_style.text)),
      m_types(motion.in.kind == inset::Motion::Kind::Type ||
              motion.out.kind == inset::Motion::Kind::Type)
{
}

exportpipe::Still TitleSource::still(int visible, bool make)
{
    const titlerender::Picture pic = make ? titlerender::render(m_style, m_fontPx, visible)
                                          : titlerender::cached(m_style, m_fontPx, visible);
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
    return still(-1, true);
}

int TitleSource::stateAt(int64_t localMs) const
{
    return m_types ? titlerender::visibleAt(m_count, m_motion, localMs, m_spanMs) : -1;
}

exportpipe::Still TitleSource::at(int64_t localMs)
{
    return still(stateAt(localMs), true);
}

exportpipe::Still TitleSource::ready(int64_t localMs)
{
    return still(stateAt(localMs), false);
}

int64_t TitleSource::nextChange(int64_t localMs)
{
    if (!m_types)
        return -1;
    // The state only changes in the In and the Out, and runs one way in
    // each: in each stretch the first time it differs is found by halving.
    const inset::MotionPhase phase = inset::motionPhase(m_motion, localMs, m_spanMs);
    const int64_t inEnd = static_cast<int64_t>(std::ceil(phase.inEndMs)) - 1;
    const int64_t outStart = static_cast<int64_t>(std::floor(phase.outStartMs));
    const int now = stateAt(localMs);
    int64_t from = localMs + 1;
    for (const int64_t to : {inEnd, outStart, m_spanMs}) {
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
    return -1;
}
