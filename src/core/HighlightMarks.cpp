#include "HighlightMarks.h"

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace highlight {

QList<Mark> shown(const QList<qint64> &own, const QList<qint64> &hidden,
                  const QList<qint64> &camera)
{
    QList<Mark> out;
    out.reserve(own.size() + camera.size());
    for (const qint64 ms : own)
        out.push_back({ms, true});
    for (const qint64 ms : camera)
        if (!std::binary_search(hidden.begin(), hidden.end(), ms))
            out.push_back({ms, false});
    std::stable_sort(out.begin(), out.end(),
                     [](const Mark &a, const Mark &b) { return a.sourceMs < b.sourceMs; });
    return out;
}

bool add(QList<qint64> &own, QList<qint64> &hidden, qint64 ms, qint64 durationMs,
         qint64 frameMs, const QList<qint64> &camera)
{
    ms = std::clamp<qint64>(ms, 0, std::max<qint64>(0, durationMs));
    for (const Mark &m : shown(own, hidden, camera))
        if (std::abs(m.sourceMs - ms) < frameMs)
            return false; // one there already
    for (const qint64 c : camera)
        if (std::abs(c - ms) < frameMs && setHidden(hidden, c, false))
            return true;
    own.insert(std::lower_bound(own.begin(), own.end(), ms), ms);
    return true;
}

bool removeNear(QList<qint64> &own, QList<qint64> &hidden, qint64 ms, qint64 fromMs,
                qint64 toMs, const QList<qint64> &camera)
{
    std::optional<Mark> best;
    for (const Mark &m : shown(own, hidden, camera))
        if (m.sourceMs >= fromMs && m.sourceMs <= toMs &&
            std::abs(m.sourceMs - ms) <= kRemoveSnapMs &&
            (!best || std::abs(m.sourceMs - ms) < std::abs(best->sourceMs - ms)))
            best = m;
    if (!best)
        return false;
    return best->own ? own.removeOne(best->sourceMs) : setHidden(hidden, best->sourceMs, true);
}

bool setHidden(QList<qint64> &hidden, qint64 ms, bool hide)
{
    const auto at = std::lower_bound(hidden.begin(), hidden.end(), ms);
    const bool is = at != hidden.end() && *at == ms;
    if (is == hide)
        return false;
    if (hide)
        hidden.insert(at, ms);
    else
        hidden.erase(at);
    return true;
}

QList<qint64> merged(const QList<qint64> &a, const QList<qint64> &b)
{
    QList<qint64> out = a + b;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace highlight
