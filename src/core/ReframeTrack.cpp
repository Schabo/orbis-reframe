#include "ReframeTrack.h"

#include "KeyTrack.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = 2.0f * kPi;

float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// Shortest-arc interpolation: step along the signed angular difference wrapped
// into (-pi, pi], so a path crossing the equirect seam takes the short way.
float lerpAngle(float a, float b, float t)
{
    float d = std::fmod(b - a, kTwoPi);
    if (d < -kPi)
        d += kTwoPi;
    else if (d > kPi)
        d -= kTwoPi;
    return a + d * t;
}

} // namespace

QList<qint64> ReframeTrack::keyTimes() const
{
    QList<qint64> times;
    times.reserve(m_keys.size());
    for (const ReframeKey &k : m_keys)
        times.append(k.timeMs);
    return times;
}

void ReframeTrack::replaceSpan(qint64 fromMs, qint64 toMs, const QList<ReframeKey> &keys)
{
    if (toMs < fromMs)
        std::swap(fromMs, toMs);
    m_keys.removeIf([&](const ReframeKey &k) { return k.timeMs >= fromMs && k.timeMs <= toMs; });
    for (ReframeKey k : keys) {
        k.timeMs = std::clamp(k.timeMs, fromMs, toMs);
        const auto it = keytrack::lowerBound(m_keys, k.timeMs);
        if (it != m_keys.end() && it->timeMs == k.timeMs)
            it->pose = k.pose;
        else
            m_keys.insert(it, k);
    }
}

void ReframeTrack::upsert(qint64 timeMs, const ReframePose &pose, qint64 snapMs)
{
    keytrack::upsert(m_keys, ReframeKey{timeMs, pose}, snapMs);
}

bool ReframeTrack::setEase(qint64 timeMs, qint64 snapMs, keytrack::Ease ease)
{
    return keytrack::setEase(m_keys, timeMs, snapMs, ease);
}

bool ReframeTrack::removeNear(qint64 timeMs, qint64 snapMs)
{
    return keytrack::removeNear(m_keys, timeMs, snapMs);
}

ReframePose ReframeTrack::sample(qint64 timeMs) const
{
    const auto [lo, hi, t] = keytrack::bracket(
        m_keys, timeMs, [](const ReframeKey &a, const ReframeKey &b) {
            return a.pose.yaw == b.pose.yaw && a.pose.pitch == b.pose.pitch &&
                   a.pose.roll == b.pose.roll && a.pose.fovY == b.pose.fovY;
        });
    if (!lo)
        return {};
    if (lo == hi)
        return lo->pose;
    ReframePose out;
    out.yaw = lerpAngle(lo->pose.yaw, hi->pose.yaw, t);
    out.pitch = lerp(lo->pose.pitch, hi->pose.pitch, t);
    out.roll = lerpAngle(lo->pose.roll, hi->pose.roll, t);
    out.fovY = lerp(lo->pose.fovY, hi->pose.fovY, t);
    out.projection = lo->pose.projection; // a segment keeps its start key's shape
    return out;
}
