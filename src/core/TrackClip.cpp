#include "TrackClip.h"

#include "KeyTrack.h"

void TrackClip::resetToFullSource()
{
    trimInMs = 0;
    trimOutMs = std::max<qint64>(sourceDurationMs, kMinTrimMs);
    fadeInMs = 0;
    fadeOutMs = 0;
}

bool TrackClip::setStartMs(qint64 ms)
{
    ms = std::max<qint64>(0, ms);
    if (ms == startMs)
        return false;
    startMs = ms;
    return true;
}

bool TrackClip::setTrimIn(qint64 sourceMs)
{
    sourceMs = std::clamp<qint64>(sourceMs, 0, trimOutMs - kMinTrimMs);
    if (sourceMs == trimInMs)
        return false;
    // Keep the audio after the new in-point where it is on the timeline. Can't
    // start before 0: then the in-point is pulled back to where 0 lands.
    qint64 newStart = startMs + std::llround((sourceMs - trimInMs) / speed);
    if (newStart < 0) {
        sourceMs = std::clamp<qint64>(trimInMs - std::llround(startMs * speed), 0,
                                      trimOutMs - kMinTrimMs);
        newStart = 0;
        if (sourceMs == trimInMs)
            return false;
    }
    trimInMs = sourceMs;
    startMs = newStart;
    clampFades();
    return true;
}

bool TrackClip::setTrimOut(qint64 sourceMs)
{
    const qint64 maxOut = sourceDurationMs > 0 ? sourceDurationMs : trimOutMs;
    sourceMs = std::clamp<qint64>(sourceMs, trimInMs + kMinTrimMs, std::max(maxOut, trimInMs + kMinTrimMs));
    if (sourceMs == trimOutMs)
        return false;
    trimOutMs = sourceMs;
    clampFades();
    return true;
}

bool TrackClip::setGainDb(double db)
{
    db = std::clamp(db, audiolimits::kMinGainDb, audiolimits::kMaxGainDb);
    if (db == gainDb)
        return false;
    gainDb = db;
    return true;
}

bool TrackClip::setMuted(bool m)
{
    if (m == muted)
        return false;
    muted = m;
    return true;
}

bool TrackClip::setNoise(const insv::NoiseReduction &nr)
{
    const insv::NoiseReduction n = nr.clamped();
    if (n == noise)
        return false;
    noise = n;
    return true;
}

bool TrackClip::setFades(qint64 inMs, qint64 outMs)
{
    const qint64 oldIn = fadeInMs, oldOut = fadeOutMs;
    fadeInMs = std::max<qint64>(0, inMs);
    fadeOutMs = std::max<qint64>(0, outMs);
    clampFades();
    return fadeInMs != oldIn || fadeOutMs != oldOut;
}

void TrackClip::clampFades()
{
    const qint64 span = timelineDurationMs();
    fadeInMs = std::clamp<qint64>(fadeInMs, 0, span);
    fadeOutMs = std::clamp<qint64>(fadeOutMs, 0, span - fadeInMs);
}

// --- InsetKeyTrack ----------------------------------------------------------

void InsetKeyTrack::upsert(qint64 timeMs, const inset::Look &look, qint64 snapMs)
{
    keytrack::upsert(m_keys, InsetKey{timeMs, look.clamped()}, snapMs);
}

bool InsetKeyTrack::setEase(qint64 timeMs, qint64 snapMs, keytrack::Ease ease)
{
    return keytrack::setEase(m_keys, timeMs, snapMs, ease);
}

bool InsetKeyTrack::removeNear(qint64 timeMs, qint64 snapMs)
{
    return keytrack::removeNear(m_keys, timeMs, snapMs);
}

inset::Look InsetKeyTrack::sample(qint64 timeMs) const
{
    const auto [lo, hi, t] = keytrack::bracket(m_keys, timeMs);
    if (!lo)
        return {};
    if (lo == hi)
        return lo->look;
    return inset::lerp(lo->look, hi->look, t);
}

bool InsetKeyTrack::operator==(const InsetKeyTrack &o) const
{
    if (m_keys.size() != o.m_keys.size())
        return false;
    for (int i = 0; i < m_keys.size(); ++i)
        if (m_keys[i].timeMs != o.m_keys[i].timeMs || m_keys[i].look != o.m_keys[i].look ||
            m_keys[i].ease != o.m_keys[i].ease)
            return false;
    return true;
}
