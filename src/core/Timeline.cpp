#include "Timeline.h"

#include <limits>
#include <cmath>
#include <algorithm>

Timeline::Timeline(QObject *parent)
    : QObject(parent)
{
}

int Timeline::indexOfId(int id) const
{
    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].id == id)
            return i;
    }
    return -1;
}

const ClipItem *Timeline::clipById(int id) const
{
    const int i = indexOfId(id);
    return i < 0 ? nullptr : &m_clips[i];
}

ClipItem *Timeline::mutableClipById(int id)
{
    const int i = indexOfId(id);
    return i < 0 ? nullptr : &m_clips[i];
}

qint64 Timeline::totalDurationMs() const
{
    qint64 total = 0;
    for (const ClipItem &c : m_clips)
        total += c.timelineDurationMs();
    return total;
}

Timeline::Point Timeline::timelineToClip(qint64 timelineMs) const
{
    if (m_clips.isEmpty())
        return {-1, 0};

    timelineMs = std::clamp<qint64>(timelineMs, 0, totalDurationMs());
    qint64 acc = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        const qint64 span = m_clips[i].timelineDurationMs();
        // The boundary between two clips belongs to the later one; the very
        // end of the timeline resolves to the last clip at its full span.
        if (timelineMs < acc + span || i == m_clips.size() - 1)
            return {i, std::clamp<qint64>(timelineMs - acc, 0, span)};
        acc += span;
    }
    return {-1, 0}; // unreachable: the loop always returns on its last iteration
}

qint64 Timeline::clipToTimeline(int clipIndex, qint64 clipLocalMs) const
{
    if (clipIndex < 0 || clipIndex >= m_clips.size())
        return 0;
    qint64 acc = 0;
    for (int i = 0; i < clipIndex; ++i)
        acc += m_clips[i].timelineDurationMs();
    return acc + std::clamp<qint64>(clipLocalMs, 0, m_clips[clipIndex].timelineDurationMs());
}

namespace {

double framePeriodMs(const ClipItem &c)
{
    return 1000.0 / c.fps;
}

// A paused seek to source ms t shows the first frame at or after t (the
// decoders keep a frame whose pts + 1 ms reaches the target), so frame k's
// start rounded up is the one ms that shows it on either side of the
// rounding, and the frame on screen at t is the first one starting at or
// after t - 1.
qint64 frameStartMs(double periodMs, qint64 k)
{
    return static_cast<qint64>(std::ceil(static_cast<double>(k) * periodMs - 1e-6));
}

qint64 frameShownAt(double periodMs, qint64 sourceMs)
{
    return static_cast<qint64>(std::ceil((sourceMs - 1) / periodMs - 1e-6));
}

// Source ms -> clip-local timeline ms, rounded up so the controller's
// sourceAtLocalMs() of the result doesn't land before `sourceMs`.
qint64 localAtOrAfter(const ClipItem &c, qint64 sourceMs)
{
    return static_cast<qint64>(std::ceil((sourceMs - c.trimInMs) / c.speed - 1e-6));
}

} // namespace

qint64 Timeline::stepFrames(qint64 timelineMs, int frames) const
{
    const Point pt = timelineToClip(timelineMs);
    if (pt.clipIndex < 0 || frames == 0)
        return std::clamp<qint64>(timelineMs, 0, totalDurationMs());

    const ClipItem &c = m_clips[pt.clipIndex];
    const double period = framePeriodMs(c);
    const qint64 k = frameShownAt(period, c.sourceAtLocalMs(pt.clipLocalMs)) + frames;
    const qint64 targetSrc = frameStartMs(period, k);

    if (targetSrc >= c.trimOutMs) {
        if (pt.clipIndex + 1 < m_clips.size())
            return clipToTimeline(pt.clipIndex + 1, 0);
        return totalDurationMs();
    }
    if (targetSrc < c.trimInMs - 1) { // trimIn - 1 still shows the first frame
        if (pt.clipIndex == 0)
            return 0;
        // The previous clip's last frame: the last one starting before its out.
        const ClipItem &p = m_clips[pt.clipIndex - 1];
        const double pp = framePeriodMs(p);
        const qint64 last = frameShownAt(pp, p.trimOutMs + 1) - 1;
        const qint64 s = std::max(frameStartMs(pp, last), p.trimInMs);
        return clipToTimeline(pt.clipIndex - 1,
                              std::min(localAtOrAfter(p, s), p.timelineDurationMs() - 1));
    }
    return clipToTimeline(pt.clipIndex, std::max<qint64>(localAtOrAfter(c, targetSrc), 0));
}

qint64 Timeline::adjacentEditPoint(qint64 timelineMs, int direction) const
{
    const qint64 total = totalDurationMs();
    std::vector<qint64> points{0, total};
    qint64 acc = 0;
    for (const ClipItem &c : m_clips)
        points.push_back(acc += c.timelineDurationMs());
    for (const Track &t : m_tracks)
        for (const TrackClip &c : t.clips) {
            points.push_back(c.startMs);
            points.push_back(c.endMs());
        }

    qint64 best = timelineMs;
    for (qint64 p : points) {
        if (p < 0 || p > total)
            continue;
        if (direction > 0 && p > timelineMs && (best == timelineMs || p < best))
            best = p;
        else if (direction < 0 && p < timelineMs && (best == timelineMs || p > best))
            best = p;
    }
    return best;
}

qint64 Timeline::adjacentKey(qint64 timelineMs, int direction, int overlayClipId) const
{
    // Mapping a key to the timeline and back can be off by a rounded ms.
    constexpr qint64 kOnKeyMs = 5;
    std::vector<qint64> points;
    if (overlayClipId > 0) {
        if (const TrackClip *c = trackClipById(overlayClipId))
            for (const InsetKey &k : c->insetKeys.keys())
                if (k.timeMs >= c->trimInMs && k.timeMs <= c->trimOutMs)
                    points.push_back(c->startMs +
                                     std::llround((k.timeMs - c->trimInMs) / c->speed));
    } else {
        qint64 acc = 0;
        for (const ClipItem &c : m_clips) {
            // A key on the trim-out point would be the next clip's first frame.
            for (const ReframeKey &k : c.reframe.keys())
                if (k.timeMs >= c.trimInMs && k.timeMs < c.trimOutMs)
                    points.push_back(acc + c.localAtSourceMs(k.timeMs));
            acc += c.timelineDurationMs();
        }
    }

    qint64 best = timelineMs;
    for (qint64 p : points) {
        if (direction > 0 && p > timelineMs + kOnKeyMs && (best == timelineMs || p < best))
            best = p;
        else if (direction < 0 && p < timelineMs - kOnKeyMs && (best == timelineMs || p > best))
            best = p;
    }
    return best;
}

int Timeline::insertClip(int index, const QString &sourcePath, qint64 sourceDurationMs,
                         double fps, int sourceWidth, int sourceHeight)
{
    index = std::clamp(index, 0, static_cast<int>(m_clips.size()));

    ClipItem c;
    c.id = m_nextId++;
    c.sourcePath = sourcePath;
    c.sourceDurationMs = std::max<qint64>(0, sourceDurationMs);
    c.fps = fps;
    c.sourceWidth = sourceWidth;
    c.sourceHeight = sourceHeight;
    c.resetEditsToFullSource();
    if (!m_clips.isEmpty()) // the neighbour's stabilization: before it, else after it
        c.stabilization = m_clips.at(index > 0 ? index - 1 : 0).stabilization;
    for (const ClipItem &o : m_clips) // the sync fine-tune belongs to the source file
        if (o.sourcePath == sourcePath) {
            if (!std::isnan(o.syncAdjustMs))
                c.syncAdjustMs = o.syncAdjustMs;
            c.seamAdjustDeg = o.seamAdjustDeg;
            c.seamAdjustGuard = o.seamAdjustGuard;
            break;
        }
    m_clips.insert(index, c);

    emit clipInserted(index);
    emit durationChanged(totalDurationMs());
    if (relinkAudio()) // clips after it moved
        emit tracksChanged();
    return c.id;
}

int Timeline::appendClip(const QString &sourcePath, qint64 sourceDurationMs, double fps,
                         int sourceWidth, int sourceHeight)
{
    return insertClip(static_cast<int>(m_clips.size()), sourcePath, sourceDurationMs, fps,
                      sourceWidth, sourceHeight);
}

bool Timeline::removeClip(int clipId)
{
    const int i = indexOfId(clipId);
    if (i < 0)
        return false;
    const bool audio = removeLinkedAudio(clipId); // its detached mic goes with it
    m_clips.removeAt(i);
    emit clipRemoved(clipId);
    emit durationChanged(totalDurationMs());
    if (relinkAudio() || audio)
        emit tracksChanged();
    return true;
}

bool Timeline::moveClip(int clipId, int toIndex)
{
    const int from = indexOfId(clipId);
    if (from < 0)
        return false;
    toIndex = std::clamp(toIndex, 0, static_cast<int>(m_clips.size()) - 1);
    if (toIndex == from)
        return false;
    m_clips.move(from, toIndex);
    emit clipsReordered();
    if (relinkAudio())
        emit tracksChanged();
    return true;
}

int Timeline::splitClip(int clipId, qint64 atSourceMs)
{
    const int i = indexOfId(clipId);
    if (i < 0)
        return 0;
    ClipItem &left = m_clips[i];
    if (atSourceMs <= left.trimInMs + ClipItem::kMinTrimMs
        || atSourceMs >= left.trimOutMs - ClipItem::kMinTrimMs)
        return 0;

    ClipItem right = left; // same source / metadata / reframe track
    right.id = m_nextId++;
    right.trimInMs = atSourceMs;
    left.trimOutMs = atSourceMs;
    const int leftId = left.id;
    const qint64 leftIn = left.trimInMs;
    const qint64 leftOut = left.trimOutMs;

    m_clips.insert(i + 1, right); // may reallocate -- don't touch `left` after this
    const bool audio = splitLinkedAudio(leftId, right.id, atSourceMs);

    emit clipInserted(i + 1);
    emit clipTrimChanged(leftId, leftIn, leftOut);
    emit durationChanged(totalDurationMs());
    if (relinkAudio() || audio)
        emit tracksChanged();
    return right.id;
}

void Timeline::clear()
{
    setMainAudioMuted(false);
    if (!m_tracks.isEmpty()) {
        m_tracks.clear();
        emit tracksChanged();
    }
    if (m_clips.isEmpty())
        return;
    m_clips.clear();
    emit clipsReordered();
    emit durationChanged(0);
}

std::vector<Timeline::ClipPlan> Timeline::playbackPlan() const
{
    std::vector<ClipPlan> plan;
    plan.reserve(static_cast<size_t>(m_clips.size()));
    for (const ClipItem &c : m_clips)
        plan.push_back({c.sourcePath.toStdString(), c.trimInMs, c.trimOutMs});
    return plan;
}

void Timeline::setClipTrimIn(int clipId, qint64 positionMs)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->setTrimIn(positionMs)) {
        const bool audio = followTrim(clipId, oldIn, oldOut);
        emit clipTrimChanged(c->id, c->trimInMs, c->trimOutMs);
        emit durationChanged(totalDurationMs());
        if (relinkAudio() || audio)
            emit tracksChanged();
    }
}

void Timeline::setClipTrimOut(int clipId, qint64 positionMs)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->setTrimOut(positionMs)) {
        const bool audio = followTrim(clipId, oldIn, oldOut);
        emit clipTrimChanged(c->id, c->trimInMs, c->trimOutMs);
        emit durationChanged(totalDurationMs());
        if (relinkAudio() || audio)
            emit tracksChanged();
    }
}

void Timeline::clearClipTrim(int clipId)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->clearTrim()) {
        const bool audio = followTrim(clipId, oldIn, oldOut);
        emit clipTrimChanged(c->id, c->trimInMs, c->trimOutMs);
        emit durationChanged(totalDurationMs());
        if (relinkAudio() || audio)
            emit tracksChanged();
    }
}

void Timeline::upsertClipReframeKey(int clipId, qint64 sourceMs, ReframePose pose)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->upsertReframeKey(sourceMs, pose))
        emit clipReframeChanged(c->id);
}

void Timeline::removeClipReframeKeyNear(int clipId, qint64 sourceMs)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->removeReframeKeyNear(sourceMs))
        emit clipReframeChanged(c->id);
}

void Timeline::replaceClipReframeSpan(int clipId, qint64 fromMs, qint64 toMs,
                                      const QList<ReframeKey> &keys)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c)
        return;
    c->reframe.replaceSpan(fromMs, toMs, keys);
    emit clipReframeChanged(c->id);
}

void Timeline::clearClipReframe(int clipId)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->clearReframe())
        emit clipReframeChanged(c->id);
}

void Timeline::setClipAudio(int clipId, double gainDb, bool muted)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c)
        return;
    const bool gainChanged = c->setAudioGainDb(gainDb);
    const bool mutedChanged = c->setAudioMuted(muted);
    if (gainChanged || mutedChanged)
        emit clipAudioChanged(c->id, c->audioGainDb, c->audioMuted);
}

void Timeline::setClipNoise(int clipId, const insv::NoiseReduction &nr)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setNoise(nr))
        emit clipAudioChanged(c->id, c->audioGainDb, c->audioMuted);
}

void Timeline::setClipAudioTrack(int clipId, int track)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setAudioTrack(track))
        emit clipAudioChanged(c->id, c->audioGainDb, c->audioMuted);
}

void Timeline::setClipSpeed(int clipId, double speed)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setSpeed(speed)) {
        emit clipSpeedChanged(c->id, c->speed);
        emit durationChanged(totalDurationMs());
        if (relinkAudio()) // its linked audio takes the speed; later clips move
            emit tracksChanged();
    }
}

void Timeline::setClipColor(int clipId, const ColorGrade &grade)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setColorGrade(grade))
        emit clipColorChanged(c->id, c->color);
}

void Timeline::setClipStabilization(int clipId, const insv::StabConfig &config)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setStabilization(config))
        emit clipStabilizationChanged(c->id, c->stabilization);
}

void Timeline::setClipBlurPlates(int clipId, bool on)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setBlurPlates(on))
        emit clipPrivacyChanged(c->id, on);
}

void Timeline::setClipBlurAdditions(int clipId, const std::vector<privacy::BlurAddition> &additions)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setBlurAdditions(additions))
        emit clipPrivacyChanged(c->id, c->blurPlates);
}

void Timeline::setClipBlurRemovals(int clipId, const std::vector<privacy::BlurRemoval> &removals)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setBlurRemovals(removals))
        emit clipPrivacyChanged(c->id, c->blurPlates);
}

void Timeline::setSourceSyncAdjust(const QString &sourcePath, double ms)
{
    for (ClipItem &c : m_clips) {
        if (c.sourcePath != sourcePath || c.syncAdjustMs == ms)
            continue;
        c.syncAdjustMs = ms;
        emit clipSyncChanged(c.id, ms);
    }
}

void Timeline::setSourceSeamAdjust(const QString &sourcePath, const std::array<double, 3> &deg,
                                   int lensGuard)
{
    for (ClipItem &c : m_clips) {
        if (c.sourcePath != sourcePath ||
            (c.seamAdjustDeg == deg && c.seamAdjustGuard == lensGuard))
            continue;
        c.seamAdjustDeg = deg;
        c.seamAdjustGuard = lensGuard;
        emit clipSeamChanged(c.id);
    }
}

void Timeline::setClipMediaMissing(int clipId, bool missing)
{
    ClipItem *c = mutableClipById(clipId);
    if (c)
        c->mediaMissing = missing;
}

// ---------------------------------------------------------------------------
// Extra tracks
// ---------------------------------------------------------------------------

int Timeline::trackIndexOfId(int trackId) const
{
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].id == trackId)
            return i;
    return -1;
}

const Track *Timeline::trackById(int trackId) const
{
    const int i = trackIndexOfId(trackId);
    return i < 0 ? nullptr : &m_tracks[i];
}

Track *Timeline::mutableTrackById(int trackId)
{
    const int i = trackIndexOfId(trackId);
    return i < 0 ? nullptr : &m_tracks[i];
}

const TrackClip *Timeline::trackClipById(int clipId, int *trackId) const
{
    for (const Track &t : m_tracks)
        for (const TrackClip &c : t.clips)
            if (c.id == clipId) {
                if (trackId)
                    *trackId = t.id;
                return &c;
            }
    return nullptr;
}

TrackClip *Timeline::mutableTrackClipById(int clipId, Track **track)
{
    for (Track &t : m_tracks)
        for (TrackClip &c : t.clips)
            if (c.id == clipId) {
                if (track)
                    *track = &t;
                return &c;
            }
    return nullptr;
}

qint64 Timeline::extraTracksEndMs() const
{
    qint64 end = 0;
    for (const Track &t : m_tracks)
        for (const TrackClip &c : t.clips)
            end = std::max(end, c.endMs());
    return end;
}

int Timeline::addTrack(Track::Kind kind, const QString &name)
{
    Track t;
    t.id = m_nextTrackId++;
    t.kind = kind;
    t.name = name;
    if (t.name.isEmpty()) {
        int n = 1;
        for (const Track &o : m_tracks)
            if (o.kind == kind)
                ++n;
        t.name = kind == Track::Kind::Video ? tr("Overlay %1").arg(n) : tr("Audio %1").arg(n);
    }
    if (kind == Track::Kind::Video)
        m_tracks.insert(videoTrackCount(), t);
    else
        m_tracks.push_back(t);
    emit tracksChanged();
    return t.id;
}

bool Timeline::removeTrack(int trackId)
{
    const int i = trackIndexOfId(trackId);
    if (i < 0)
        return false;
    const QList<TrackClip> clips = m_tracks[i].clips;
    m_tracks.removeAt(i);
    for (const TrackClip &c : clips) // an overlay's sound goes with it
        removeLinkedAudio(c.id);
    emit tracksChanged();
    return true;
}

bool Timeline::setTrackName(int trackId, const QString &name)
{
    Track *t = mutableTrackById(trackId);
    const QString n = name.trimmed();
    if (!t || n.isEmpty() || n == t->name)
        return false;
    t->name = n;
    emit tracksChanged();
    return true;
}

bool Timeline::moveTrack(int trackId, int toIndex)
{
    const int from = trackIndexOfId(trackId);
    if (from < 0)
        return false;
    const int videos = videoTrackCount();
    if (m_tracks[from].kind == Track::Kind::Video)
        toIndex = std::clamp(toIndex, 0, videos - 1);
    else
        toIndex = std::clamp(toIndex, videos, static_cast<int>(m_tracks.size()) - 1);
    if (toIndex == from)
        return false;
    m_tracks.move(from, toIndex);
    emit tracksChanged();
    return true;
}

void Timeline::setMainAudioMuted(bool muted)
{
    if (m_mainAudioMuted == muted)
        return;
    m_mainAudioMuted = muted;
    emit mainAudioMutedChanged(muted);
}

void Timeline::setTrackAudio(int trackId, double gainDb, bool muted)
{
    Track *t = mutableTrackById(trackId);
    if (!t)
        return;
    gainDb = std::clamp(gainDb, audiolimits::kMinGainDb, audiolimits::kMaxGainDb);
    if (t->gainDb == gainDb && t->muted == muted)
        return;
    t->gainDb = gainDb;
    t->muted = muted;
    emit tracksChanged();
}

int Timeline::insertTrackClip(int trackId, const TrackClip &clip)
{
    Track *t = mutableTrackById(trackId);
    if (!t)
        return 0;
    TrackClip c = clip;
    c.id = m_nextId++;
    c.sourceDurationMs = std::max<qint64>(0, c.sourceDurationMs);
    c.speed = std::clamp(c.speed > 0.0 ? c.speed : 1.0, ClipItem::kMinSpeed, ClipItem::kMaxSpeed);
    c.startMs = std::max<qint64>(0, c.startMs);
    // Trims into range with the min gap (the saved pair, or the whole source).
    const qint64 srcEnd = std::max<qint64>(c.sourceDurationMs, TrackClip::kMinTrimMs);
    if (c.trimOutMs <= 0)
        c.trimOutMs = srcEnd;
    c.trimOutMs = std::clamp<qint64>(c.trimOutMs, TrackClip::kMinTrimMs, srcEnd);
    c.trimInMs = std::clamp<qint64>(c.trimInMs, 0, c.trimOutMs - TrackClip::kMinTrimMs);
    c.gainDb = std::clamp(c.gainDb, audiolimits::kMinGainDb, audiolimits::kMaxGainDb);
    c.setFades(c.fadeInMs, c.fadeOutMs);
    c.look = c.look.clamped();
    c.frame = c.frame.clamped();
    if (t->kind == Track::Kind::Video) { // no audio link on an overlay
        c.linkedClipId = 0;
        c.stream = -1;
    }
    if (c.linkedClipId && !linkAnchor(c.linkedClipId))
        c.linkedClipId = 0;
    t->clips.push_back(c);
    t->sortClips();
    relinkAudio();
    emit tracksChanged();
    return c.id;
}

int Timeline::addAudioClip(int trackId, const QString &sourcePath, qint64 sourceDurationMs,
                           qint64 startMs, int stream)
{
    const Track *t = trackById(trackId);
    if (!t || t->kind != Track::Kind::Audio)
        return 0;
    TrackClip c;
    c.sourcePath = sourcePath;
    c.sourceDurationMs = sourceDurationMs;
    c.stream = stream;
    c.startMs = startMs;
    c.resetToFullSource();
    return insertTrackClip(trackId, c);
}

int Timeline::addVideoClip(int trackId, const QString &sourcePath, qint64 sourceDurationMs,
                           qint64 startMs, int sourceWidth, int sourceHeight)
{
    const Track *t = trackById(trackId);
    if (!t || t->kind != Track::Kind::Video)
        return 0;
    TrackClip c;
    c.sourcePath = sourcePath;
    c.sourceDurationMs = sourceDurationMs;
    c.startMs = startMs;
    c.sourceWidth = sourceWidth;
    c.sourceHeight = sourceHeight;
    c.resetToFullSource();
    return insertTrackClip(trackId, c);
}

bool Timeline::removeTrackClip(int clipId)
{
    for (Track &t : m_tracks)
        for (int i = 0; i < t.clips.size(); ++i)
            if (t.clips[i].id == clipId) {
                t.clips.removeAt(i);
                removeLinkedAudio(clipId); // an overlay's sound goes with it
                emit tracksChanged();
                return true;
            }
    return false;
}

void Timeline::moveTrackClip(int clipId, int toTrackId, qint64 startMs)
{
    Track *from = nullptr;
    TrackClip *c = mutableTrackClipById(clipId, &from);
    Track *to = mutableTrackById(toTrackId);
    if (!c || !to || to->kind != from->kind)
        return;
    if (c->linkedClipId) // locked in time to its video clip: only the track changes
        startMs = c->startMs;
    if (from == to) {
        if (c->setStartMs(startMs)) {
            from->sortClips();
            relinkAudio(); // an overlay's sound moves with it
            emit tracksChanged();
        }
        return;
    }
    TrackClip moved = *c;
    moved.setStartMs(startMs);
    from->clips.removeIf([clipId](const TrackClip &a) { return a.id == clipId; });
    to->clips.push_back(moved);
    to->sortClips();
    relinkAudio();
    emit tracksChanged();
}

int Timeline::splitTrackClip(int clipId, qint64 atMs)
{
    Track *t = nullptr;
    TrackClip *c = mutableTrackClipById(clipId, &t);
    if (!c)
        return 0;
    const qint64 atSrc = c->sourceAtLocalMs(atMs - c->startMs);
    if (atSrc <= c->trimInMs + TrackClip::kMinTrimMs ||
        atSrc >= c->trimOutMs - TrackClip::kMinTrimMs)
        return 0;
    TrackClip right = *c;
    right.id = m_nextId++;
    const int leftId = c->id;
    right.startMs = c->startMs + std::llround((atSrc - c->trimInMs) / c->speed);
    right.trimInMs = atSrc;
    right.fadeInMs = 0;
    c->trimOutMs = atSrc;
    c->fadeOutMs = 0;
    c->setFades(c->fadeInMs, 0);     // clamp to the shorter span
    right.setFades(0, right.fadeOutMs);
    t->clips.push_back(right); // may reallocate -- don't touch `c` after this
    t->sortClips();
    if (t->kind == Track::Kind::Video) // an overlay's sound splits with it
        splitLinkedAudio(leftId, right.id, atSrc);
    relinkAudio();
    emit tracksChanged();
    return right.id;
}

void Timeline::setTrackClipTrimIn(int clipId, qint64 sourceMs)
{
    Track *t = nullptr;
    TrackClip *c = mutableTrackClipById(clipId, &t);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->setTrimIn(sourceMs)) {
        t->sortClips();
        if (t->kind == Track::Kind::Video) // an overlay's sound follows the trim
            followTrim(clipId, oldIn, oldOut);
        relinkAudio(); // a linked clip's start is derived from its in-point
        emit tracksChanged();
    }
}

void Timeline::setTrackClipTrimOut(int clipId, qint64 sourceMs)
{
    Track *t = nullptr;
    TrackClip *c = mutableTrackClipById(clipId, &t);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->setTrimOut(sourceMs)) {
        if (t->kind == Track::Kind::Video) {
            followTrim(clipId, oldIn, oldOut);
            relinkAudio();
        }
        emit tracksChanged();
    }
}

void Timeline::setAudioClipNoise(int clipId, const insv::NoiseReduction &nr)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (c && c->setNoise(nr))
        emit tracksChanged();
}

void Timeline::setAudioClipLevel(int clipId, double gainDb, bool muted)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c)
        return;
    const bool g = c->setGainDb(gainDb);
    const bool m = c->setMuted(muted);
    if (g || m)
        emit tracksChanged();
}

void Timeline::setTrackClipFades(int clipId, qint64 fadeInMs, qint64 fadeOutMs)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (c && c->setFades(fadeInMs, fadeOutMs))
        emit tracksChanged();
}

void Timeline::setTrackClipInset(int clipId, const inset::Look &look, const inset::Frame &frame)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c)
        return;
    const inset::Look l = look.clamped();
    const inset::Frame f = frame.clamped();
    if (l == c->look && f == c->frame)
        return;
    c->look = l;
    c->frame = f;
    emit tracksChanged();
}

void Timeline::upsertTrackClipInsetKey(int clipId, qint64 sourceMs, const inset::Look &look)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c)
        return;
    const InsetKeyTrack before = c->insetKeys;
    c->insetKeys.upsert(sourceMs, look, kInsetKeySnapMs);
    if (!(c->insetKeys == before))
        emit tracksChanged();
}

void Timeline::removeTrackClipInsetKeyNear(int clipId, qint64 sourceMs)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c || c->insetKeys.count() == 0)
        return;
    const inset::Look last = c->insetKeys.sample(sourceMs);
    if (!c->insetKeys.removeNear(sourceMs, kInsetKeyDeleteSnapMs))
        return;
    if (c->insetKeys.isEmpty())
        c->look = last; // the last key's look stays where it was
    emit tracksChanged();
}

void Timeline::clearTrackClipInsetKeys(int clipId, qint64 sourceMs)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c || c->insetKeys.isEmpty())
        return;
    c->look = c->insetKeys.sample(sourceMs);
    c->insetKeys.clear();
    emit tracksChanged();
}

int Timeline::videoTrackCount() const
{
    int n = 0;
    while (n < m_tracks.size() && m_tracks[n].kind == Track::Kind::Video)
        ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Linked audio (to main-track or overlay clips)
// ---------------------------------------------------------------------------

bool Timeline::linkAnchor(int clipId, LinkAnchor *anchor) const
{
    LinkAnchor a;
    const int idx = indexOfId(clipId);
    if (idx >= 0) {
        const ClipItem &c = m_clips[idx];
        a = {clipToTimeline(idx, 0), c.trimInMs, c.trimOutMs, c.speed};
    } else {
        int trackId = 0;
        const TrackClip *c = trackClipById(clipId, &trackId);
        if (!c || trackById(trackId)->kind != Track::Kind::Video)
            return false;
        a = {c->startMs, c->trimInMs, c->trimOutMs, c->speed};
    }
    if (anchor)
        *anchor = a;
    return true;
}

bool Timeline::linkAudioClip(int audioClipId, int videoClipId)
{
    Track *t = nullptr;
    TrackClip *a = mutableTrackClipById(audioClipId, &t);
    if (!a || t->kind != Track::Kind::Audio)
        return false;
    if (videoClipId == 0) {
        if (!a->linkedClipId)
            return true;
        a->linkedClipId = 0;
        a->syncOffsetMs = 0;
        emit tracksChanged();
        return true;
    }
    LinkAnchor v;
    if (!linkAnchor(videoClipId, &v))
        return false;
    a->linkedClipId = videoClipId;
    a->speed = v.speed;
    // Where the link alone would put it; the difference is the sync offset,
    // so linking doesn't move anything.
    a->syncOffsetMs =
        a->startMs - (v.startMs + std::llround((a->trimInMs - v.trimInMs) / v.speed));
    relinkAudio();
    emit tracksChanged();
    return true;
}

bool Timeline::relinkAudio()
{
    bool changed = false;
    for (Track &t : m_tracks) {
        bool moved = false;
        for (TrackClip &a : t.clips) {
            if (!a.linkedClipId)
                continue;
            LinkAnchor v;
            if (!linkAnchor(a.linkedClipId, &v)) { // its clip is gone (shouldn't
                a.linkedClipId = 0;                // happen: removal takes the audio)
                a.syncOffsetMs = 0;
                changed = true;
                continue;
            }
            if (a.speed != v.speed) {
                a.speed = v.speed;
                a.setFades(a.fadeInMs, a.fadeOutMs); // the span changed
                changed = true;
            }
            const qint64 start = std::max<qint64>(
                0, v.startMs + std::llround((a.trimInMs - v.trimInMs) / v.speed) + a.syncOffsetMs);
            if (start != a.startMs) {
                a.startMs = start;
                moved = changed = true;
            }
        }
        if (moved)
            t.sortClips();
    }
    return changed;
}

bool Timeline::followTrim(int videoClipId, qint64 oldIn, qint64 oldOut)
{
    LinkAnchor v;
    if (!linkAnchor(videoClipId, &v))
        return false;
    bool changed = false;
    for (Track &t : m_tracks) {
        for (int i = t.clips.size() - 1; i >= 0; --i) {
            TrackClip &a = t.clips[i];
            if (a.linkedClipId != videoClipId)
                continue;
            qint64 in = a.trimInMs == oldIn ? v.trimInMs : std::max(a.trimInMs, v.trimInMs);
            qint64 out = a.trimOutMs == oldOut ? v.trimOutMs : std::min(a.trimOutMs, v.trimOutMs);
            in = std::max<qint64>(0, in);
            if (a.sourceDurationMs > 0)
                out = std::min(out, a.sourceDurationMs);
            if (in == a.trimInMs && out == a.trimOutMs)
                continue;
            changed = true;
            if (out - in < TrackClip::kMinTrimMs) { // trimmed away with the video
                t.clips.removeAt(i);
                continue;
            }
            a.trimInMs = in;
            a.trimOutMs = out;
            a.setFades(a.fadeInMs, a.fadeOutMs);
        }
    }
    return changed;
}

bool Timeline::splitLinkedAudio(int leftId, int rightId, qint64 atSourceMs)
{
    bool changed = false;
    for (Track &t : m_tracks) {
        QList<TrackClip> added;
        for (TrackClip &a : t.clips) {
            if (a.linkedClipId != leftId || a.trimOutMs - atSourceMs < TrackClip::kMinTrimMs)
                continue; // (all or nearly all before the cut: stays with the left clip)
            changed = true;
            if (atSourceMs - a.trimInMs < TrackClip::kMinTrimMs) {
                a.linkedClipId = rightId; // all after the cut
                continue;
            }
            TrackClip r = a;
            r.id = m_nextId++;
            r.linkedClipId = rightId;
            r.trimInMs = atSourceMs;
            r.setFades(0, a.fadeOutMs);
            a.trimOutMs = atSourceMs;
            a.setFades(a.fadeInMs, 0);
            added.push_back(r);
        }
        t.clips.append(added);
    }
    return changed;
}

bool Timeline::removeLinkedAudio(int videoClipId)
{
    bool changed = false;
    for (Track &t : m_tracks)
        changed |= t.clips.removeIf([videoClipId](const TrackClip &a) {
            return a.linkedClipId == videoClipId;
        }) > 0;
    return changed;
}
