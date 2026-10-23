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

// The part of a segment a move takes to get up to speed from rest, or to
// come to rest at its end. The speed follows an S-curve over it (smoothstep):
// it leaves 0 and reaches the pace with no acceleration, so neither end of
// the ramp gives a jolt. It covers the same distance as an even ramp would.
constexpr float kSoftRamp = 0.25f;

// How far along a segment the value is at `t` (0..1 of its time), 0..1.
// `softStart` / `softEnd`: the move starts / ends at rest there (see
// bracket()), so instead of jumping to its pace it ramps up from 0 over the
// first kSoftRamp of the segment, or down to 0 over the last. The ease sets
// the speed between: even (Linear), falling to 0 at the end (Out), rising
// from 0 (In) -- so Out always ends at rest and In always starts at rest.
// Without either, as plain linear / quadratic.
inline float eased(Ease e, float t, bool softStart = false, bool softEnd = false)
{
    t = std::clamp(t, 0.0f, 1.0f);
    // The speed over the segment through these points (unscaled): from each
    // to the next along a straight line, or an S-curve for a ramp. The
    // position is its integral up to t over the whole.
    struct Pt {
        float t, v;
        bool ramp = false; // the piece from here to the next point
    };
    Pt pts[4];
    int n = 0;
    const float r = kSoftRamp;
    switch (e) {
    case Ease::Linear:
        pts[n++] = {0.0f, softStart ? 0.0f : 1.0f, softStart};
        if (softStart)
            pts[n++] = {r, 1.0f};
        if (softEnd)
            pts[n++] = {1.0f - r, 1.0f, true};
        pts[n++] = {1.0f, softEnd ? 0.0f : 1.0f};
        break;
    case Ease::Out: // speed 1 - t
        pts[n++] = {0.0f, softStart ? 0.0f : 1.0f, softStart};
        if (softStart)
            pts[n++] = {r, 1.0f - r};
        pts[n++] = {1.0f, 0.0f};
        break;
    case Ease::In: // speed t
        pts[n++] = {0.0f, 0.0f};
        if (softEnd)
            pts[n++] = {1.0f - r, 1.0f - r, true};
        pts[n++] = {1.0f, softEnd ? 0.0f : 1.0f};
        break;
    }
    // A piece's distance from its start to `u` (0..1 of it). The speed goes
    // a.v + (b.v - a.v) * s(u): s(u) = u (straight) or 3u^2 - 2u^3 (S-curve),
    // whose integrals are u^2 / 2 and u^3 - u^4 / 2 -- both 1/2 at u = 1.
    const auto dist = [](const Pt &a, const Pt &b, float u) {
        const float grow = a.ramp ? u * u * u - u * u * u * u * 0.5f : u * u * 0.5f;
        return (b.t - a.t) * (a.v * u + (b.v - a.v) * grow);
    };
    float total = 0.0f, upTo = 0.0f;
    for (int i = 0; i + 1 < n; ++i) {
        const Pt &a = pts[i], &b = pts[i + 1];
        total += dist(a, b, 1.0f);
        if (t >= b.t)
            upTo += dist(a, b, 1.0f);
        else if (t > a.t)
            upTo += dist(a, b, (t - a.t) / (b.t - a.t));
    }
    return t >= 1.0f ? 1.0f : upTo / total;
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

// Remove the keys outside [fromMs, toMs] (a trim or split left them out of
// the clip); keys on either end stay. True if any was removed.
template <class Key>
bool removeOutside(QList<Key> &keys, qint64 fromMs, qint64 toMs)
{
    return keys.removeIf([&](const Key &k) { return k.timeMs < fromMs || k.timeMs > toMs; }) > 0;
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

// Whether the value is at rest at key `k`: the motion starts or stops there
// rather than carrying on through it. So at the first and last key, next to
// a hold (a key with the same value as `k` -- `same(a, b)` compares two keys'
// values), and where the move into `k` is Out or the move out of it is In
// (both reach 0 there).
template <class Key, class Same>
bool atRest(const QList<Key> &keys, int k, Same same)
{
    if (k <= 0 || k >= keys.size() - 1)
        return true;
    return same(keys[k - 1], keys[k]) || same(keys[k], keys[k + 1]) ||
           keys[k - 1].ease == Ease::Out || keys[k].ease == Ease::In;
}

// The keys to blend at timeMs: lo + (hi - lo) * t, t already eased by lo's
// ease, softened where the move starts or stops at rest (atRest). Before the
// first key and after the last, lo == hi (the value is held). Both null if
// `keys` is empty.
template <class Key>
struct Bracket {
    const Key *lo = nullptr;
    const Key *hi = nullptr;
    float t = 0.0f;
};

template <class Key, class Same>
Bracket<Key> bracket(const QList<Key> &keys, qint64 timeMs, Same same)
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
    const int i = static_cast<int>(it - keys.begin()) - 1;
    return {&lo, &hi, eased(lo.ease, t, atRest(keys, i, same), atRest(keys, i + 1, same))};
}

} // namespace keytrack
