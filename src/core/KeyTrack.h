#pragma once

#include <QList>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

// Search / insert / remove on a keyframe list sorted by time with unique
// times, and the easing between keys -- the part ReframeTrack and
// InsetKeyTrack share. `Key` is any struct with a `qint64 timeMs` and an
// `Ease ease`; the value it carries and how two values blend stay with each
// track.
namespace keytrack {

// How the value moves from a key to the next one (the key's own setting, for
// the segment after it): at an even pace, starting fast and slowing into the
// next key, or starting slowly and speeding up. Quadratic, so the slow end
// starts / stops at rest and the fast end moves at twice the even pace.
enum class Ease : std::uint8_t { Linear, Out, In };

inline float eased(Ease e, float t)
{
    switch (e) {
    case Ease::Out:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case Ease::In:
        return t * t;
    case Ease::Linear:
        break;
    }
    return t;
}

// The project file's names; Linear is written as no field at all.
inline const char *easeName(Ease e)
{
    switch (e) {
    case Ease::Out:
        return "easeOut";
    case Ease::In:
        return "easeIn";
    case Ease::Linear:
        break;
    }
    return "linear";
}

inline Ease easeFromName(std::string_view name)
{
    if (name == "easeOut")
        return Ease::Out;
    if (name == "easeIn")
        return Ease::In;
    return Ease::Linear; // "linear", absent, or a name from a newer build
}

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
// (which keeps its own time and ease) -- re-authoring at "the same" playhead
// frame updates rather than piles up near-duplicates.
template <class Key>
void upsert(QList<Key> &keys, Key key, qint64 snapMs)
{
    const int near = indexNear(keys, key.timeMs, snapMs);
    if (near >= 0) {
        key.timeMs = keys[near].timeMs;
        key.ease = keys[near].ease;
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

// The key whose segment (to the next key) timeMs is in: the key within snapMs
// of it, else the last key before it. -1 before the first key, at or after
// the last, or with no keys -- there's no move there to ease.
template <class Key>
int segmentAt(const QList<Key> &keys, qint64 timeMs, qint64 snapMs)
{
    int i = indexNear(keys, timeMs, snapMs);
    if (i < 0) {
        const auto it = std::upper_bound(keys.begin(), keys.end(), timeMs,
                                         [](qint64 t, const Key &k) { return t < k.timeMs; });
        i = static_cast<int>(it - keys.begin()) - 1;
    }
    return i >= 0 && i + 1 < keys.size() ? i : -1;
}

// Set the ease of the segment at timeMs (segmentAt). True if it changed.
template <class Key>
bool setEase(QList<Key> &keys, qint64 timeMs, qint64 snapMs, Ease ease)
{
    const int i = segmentAt(keys, timeMs, snapMs);
    if (i < 0 || keys[i].ease == ease)
        return false;
    keys[i].ease = ease;
    return true;
}

// The keys to blend at timeMs: lo + (hi - lo) * t, t already eased by lo's
// ease. Before the first key and after the last, lo == hi (the value is
// held). Both null if `keys` is empty.
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
    return {&lo, &hi, eased(lo.ease, t)};
}

} // namespace keytrack
