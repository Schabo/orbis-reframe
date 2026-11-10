#include "ClipItem.h"

#include <algorithm>
#include <cmath>

void ColorGrade::clamp()
{
    exposureEv = std::clamp(exposureEv, kMinExposureEv, kMaxExposureEv);
    contrast = std::clamp(contrast, kMinPercent, kMaxPercent);
    saturation = std::clamp(saturation, kMinPercent, kMaxPercent);
    temperature = std::clamp(temperature, kMinPercent, kMaxPercent);
    lutStrength = std::clamp(lutStrength, 0.0f, kMaxPercent);
}

color::Grade ColorGrade::render(std::string *lutError) const
{
    color::Grade g;
    g.adjust = packed();
    if (!lut.isEmpty()) {
        g.lut = color::cachedCube(lut.toStdString(), lutError);
        g.lutMix = lutStrength / 100.0f;
    }
    return g;
}

qint64 ClipItem::frameDurationMs() const
{
    return std::max<qint64>(1, std::llround(1000.0 / fps));
}

void ClipItem::resetEditsToFullSource()
{
    trimInMs = 0;
    trimOutMs = sourceDurationMs;
    reframe.clear();
    for (ReframeTrack &t : panes)
        t.clear();
}

bool ClipItem::setRange(qint64 inMs, qint64 outMs)
{
    // A clip shorter than kMinTrimMs can't satisfy the gap; just span it whole.
    if (sourceDurationMs <= kMinTrimMs) {
        inMs = 0;
        outMs = sourceDurationMs;
    } else {
        inMs = std::clamp<qint64>(inMs, 0, sourceDurationMs - kMinTrimMs);
        outMs = std::clamp<qint64>(outMs, inMs + kMinTrimMs, sourceDurationMs);
    }
    if (inMs == trimInMs && outMs == trimOutMs)
        return false;
    trimInMs = inMs;
    trimOutMs = outMs;
    return true;
}

bool ClipItem::setTrimIn(qint64 positionMs)
{
    const qint64 clamped = std::clamp<qint64>(positionMs, 0, sourceDurationMs);
    return setRange(std::min(clamped, trimOutMs - kMinTrimMs), trimOutMs);
}

bool ClipItem::setTrimOut(qint64 positionMs)
{
    const qint64 clamped = std::clamp<qint64>(positionMs, 0, sourceDurationMs);
    return setRange(trimInMs, std::max(clamped, trimInMs + kMinTrimMs));
}

bool ClipItem::clearTrim()
{
    return setRange(0, sourceDurationMs);
}

bool ClipItem::upsertReframeKey(qint64 sourceMs, const ReframePose &pose, int pane)
{
    if (flat)
        return false; // a flat clip isn't reframed
    // upsert always lands a key (insert, or overwrite an existing pose).
    paneTrack(pane).upsert(sourceMs, pose, frameDurationMs());
    return true;
}

bool ClipItem::removeKeysOutsideTrim()
{
    bool removed = reframe.removeOutside(trimInMs, trimOutMs);
    for (ReframeTrack &t : panes)
        removed = t.removeOutside(trimInMs, trimOutMs) || removed;
    return removed;
}

bool ClipItem::removeReframeKeyNear(qint64 sourceMs, int pane)
{
    return paneTrack(pane).removeNear(sourceMs, kReframeDeleteSnapMs);
}

bool ClipItem::clearReframe(int pane)
{
    ReframeTrack &t = paneTrack(pane);
    if (t.isEmpty())
        return false;
    t.clear();
    return true;
}

bool ClipItem::setSplit(const pane::Split &s)
{
    const pane::Split c = s.clamped();
    if (flat || c == split)
        return false;
    split = c;
    return true;
}

bool ClipItem::setSpeed(double s)
{
    if (!std::isfinite(s))
        return false;
    s = std::clamp(s, kMinSpeed, kMaxSpeed);
    if (std::abs(s - speed) < 1e-9)
        return false;
    speed = s;
    return true;
}

bool ClipItem::setFit(Fit f)
{
    if (fit == f)
        return false;
    fit = f;
    return true;
}

bool ClipItem::setBlurPlates(bool on)
{
    if (flat)
        return false; // the blur works on the sphere
    if (blurPlates == on)
        return false;
    blurPlates = on;
    return true;
}

bool ClipItem::setPlateScan(const QString &key)
{
    if (flat || plateScan == key)
        return false;
    plateScan = key;
    return true;
}

bool ClipItem::setBlurAdditions(const std::vector<privacy::BlurAddition> &additions)
{
    if (flat)
        return false;
    if (blurAdditions == additions)
        return false;
    blurAdditions = additions;
    return true;
}

bool ClipItem::setBlurRemovals(const std::vector<privacy::BlurRemoval> &removals)
{
    if (flat)
        return false;
    if (blurRemovals == removals)
        return false;
    blurRemovals = removals;
    return true;
}

bool ClipItem::setColorGrade(const ColorGrade &grade)
{
    ColorGrade clamped = grade;
    clamped.clamp();
    if (clamped == color)
        return false;
    color = clamped;
    return true;
}

bool ClipItem::setStabilization(const insv::StabConfig &config)
{
    if (flat)
        return false; // no gyro data
    if (config == stabilization)
        return false;
    stabilization = config;
    return true;
}

QList<ClipItem::Mark> ClipItem::shownMarks(const QList<qint64> &cameraMarks) const
{
    QList<Mark> out;
    out.reserve(marks.size() + (flat ? 0 : cameraMarks.size()));
    for (const qint64 ms : marks)
        out.push_back({ms, true});
    if (!flat)
        for (const qint64 ms : cameraMarks)
            if (!std::binary_search(hiddenMarks.begin(), hiddenMarks.end(), ms))
                out.push_back({ms, false});
    std::stable_sort(out.begin(), out.end(),
                     [](const Mark &a, const Mark &b) { return a.sourceMs < b.sourceMs; });
    return out;
}

bool ClipItem::addMark(qint64 sourceMs, const QList<qint64> &cameraMarks)
{
    sourceMs = std::clamp<qint64>(sourceMs, 0, sourceDurationMs);
    const qint64 frame = frameDurationMs();
    for (const Mark &m : shownMarks(cameraMarks))
        if (std::abs(m.sourceMs - sourceMs) < frame)
            return false; // one there already
    if (!flat)
        for (const qint64 ms : cameraMarks)
            if (std::abs(ms - sourceMs) < frame && setMarkHidden(ms, false))
                return true;
    marks.insert(std::lower_bound(marks.begin(), marks.end(), sourceMs), sourceMs);
    return true;
}

bool ClipItem::removeMarkNear(qint64 sourceMs, const QList<qint64> &cameraMarks)
{
    const Mark *best = nullptr;
    const QList<Mark> shown = shownMarks(cameraMarks);
    for (const Mark &m : shown)
        if (m.sourceMs >= trimInMs && m.sourceMs <= trimOutMs &&
            std::abs(m.sourceMs - sourceMs) <= kReframeDeleteSnapMs &&
            (!best || std::abs(m.sourceMs - sourceMs) < std::abs(best->sourceMs - sourceMs)))
            best = &m;
    if (!best)
        return false;
    return best->own ? removeMark(best->sourceMs) : setMarkHidden(best->sourceMs, true);
}

bool ClipItem::removeMark(qint64 sourceMs)
{
    return marks.removeOne(sourceMs);
}

bool ClipItem::setMarkHidden(qint64 sourceMs, bool hidden)
{
    if (flat)
        return false; // no camera marks
    const auto at = std::lower_bound(hiddenMarks.begin(), hiddenMarks.end(), sourceMs);
    const bool is = at != hiddenMarks.end() && *at == sourceMs;
    if (is == hidden)
        return false;
    if (hidden)
        hiddenMarks.insert(at, sourceMs);
    else
        hiddenMarks.erase(at);
    return true;
}

bool ClipItem::showHiddenMarks()
{
    if (hiddenMarks.isEmpty())
        return false;
    hiddenMarks.clear();
    return true;
}
