#pragma once

#include <QList>

#include <algorithm>
#include <cstdlib>

// Search / insert / remove on a keyframe list sorted by time with unique
// times -- the part ReframeTrack and InsetKeyTrack share. `Key` is any struct
// with a `qint64 timeMs`; the value it carries and how two values blend stay
// with each track.
namespace keytrack {

// Index of the key nearest timeMs if within snapMs, or -1.
template <class Key>
int indexNear(const QList<Key> &keys, qint64 timeMs, qint64 snapMs)
{
    int best = -1;
    qint64 bestDist = snapMs + 1;
    for (int i = 0; i < keys.size(); ++i) {
        const qint64 dist = std::llabs(keys[i].timeMs - timeMs);
        if (dist <= snapMs && dist < bestDist) {
            best = i;
            bestDist = dist;
        }
    }
    return best;
}

// First key at or after timeMs (where a key at timeMs goes).
template <class Key>
typename QList<Key>::iterator lowerBound(QList<Key> &keys, qint64 timeMs)
{
    return std::lower_bound(keys.begin(), keys.end(), timeMs,
                            [](const Key &k, qint64 t) { return k.timeMs < t; });
}

// Insert `key`, or give its value to the key within snapMs of key.timeMs
// (which keeps its own time) -- re-authoring at "the same" playhead frame
// updates rather than piles up near-duplicates.
template <class Key>
void upsert(QList<Key> &keys, Key key, qint64 snapMs)
{
    const int near = indexNear(keys, key.timeMs, snapMs);
    if (near >= 0) {
        key.timeMs = keys[near].timeMs;
        keys[near] = key;
        return;
    }
    keys.insert(lowerBound(keys, key.timeMs), key);
}

// Remove the key nearest timeMs if within snapMs. True if one was removed.
template <class Key>
bool removeNear(QList<Key> &keys, qint64 timeMs, qint64 snapMs)
{
    const int near = indexNear(keys, timeMs, snapMs);
    if (near < 0)
        return false;
    keys.removeAt(near);
    return true;
}

// The keys to blend at timeMs: lo + (hi - lo) * t. Before the first key and
// after the last, lo == hi (the value is held). Both null if `keys` is empty.
template <class Key>
struct Bracket {
    const Key *lo = nullptr;
    const Key *hi = nullptr;
    float t = 0.0f;
};

template <class Key>
Bracket<Key> bracket(const QList<Key> &keys, qint64 timeMs)
{
    if (keys.isEmpty())
        return {};
    if (timeMs <= keys.first().timeMs)
        return {&keys.first(), &keys.first(), 0.0f};
    if (timeMs >= keys.last().timeMs)
        return {&keys.last(), &keys.last(), 0.0f};
    // First key strictly after timeMs; the bracket is [it-1, it].
    const auto it = std::upper_bound(keys.begin(), keys.end(), timeMs,
                                     [](qint64 t, const Key &k) { return t < k.timeMs; });
    const Key &lo = *(it - 1);
    const Key &hi = *it;
    const qint64 span = hi.timeMs - lo.timeMs;
    const float t = span > 0 ? static_cast<float>(timeMs - lo.timeMs) / static_cast<float>(span)
                             : 0.0f;
    return {&lo, &hi, t};
}

} // namespace keytrack
