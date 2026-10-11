#include "PlaybackController.h"

#include "../render/EquirectViewportWindow.h"

#include <NativeStitchSource.h> // stitch::MakeStitchSource

#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace {
// How many decoded frames to buffer ahead during playback: up to kQueueMax
// (~0.4 s at 30 fps, enough to ride out a decode hiccup), but no more than
// kQueueBudgetBytes of RGBA -- a 5760x2880 Auto preview frame is 66 MB, so
// that's 7 frames, and 4 at 7680. See queueCapFor().
constexpr size_t kQueueMax = 12;
constexpr size_t kQueueMin = 4;
constexpr size_t kQueueBudgetBytes = size_t(512) << 20;
// While paused/scrubbing there's no clock to run ahead of, so only decode a
// frame or two past the one being shown.
constexpr size_t kScrubLookahead = 2;
// Treat "within one frame of the end" as the end.
constexpr qint64 kEndEpsilonMs = 20;
// A seek target this close to a queued frame's time is that frame: the two
// round a frame's start differently (Timeline::stepFrames rounds up, the
// decoder's pts to nearest).
constexpr qint64 kSameFrameMs = 2;
// A scrub this little ahead of the decoder decodes forward to the exact frame
// (~8 ms a frame at 8K) instead of landing on a keyframe.
constexpr qint64 kScrubDecodeAheadMs = 300;
// A scrub's pointer resting this long gets the exact frame (settle()).
constexpr int kScrubSettleMs = 150;
// A scrub going back stops on a keyframe only if it's this far behind where
// the decoder is (which runs a frame or two past the frame on screen).
constexpr qint64 kScrubHoldMs = 100;
// Present cadence. Decoupled from decode / clip frame rate -- it just governs
// how often the wall clock is checked and the shown frame swapped.
constexpr int kPresentIntervalMs = 16;
// After a seek, play, or stall the clock stays frozen until this many frames
// are queued (~0.27 s at 30 fps), not just the first one: a stitch that has
// just warmed up runs barely ahead of real time, and resuming at once made it
// run dry again tick after tick -- a stuttering picture, and choppy audio --
// for about a second after every seek.
constexpr size_t kResumeFrames = 8;

// Lens frames are ring slots on the GPU, not RAM: queue at most this many,
// which with what the viewport holds stays inside the source's ring
// (VulkanFisheyeStitcher, 10 slots before it waits).
constexpr size_t kLensQueueMax = 6;

// Playback queue length for frames of `frameBytes` (0 = none decoded yet).
size_t queueCapFor(size_t frameBytes)
{
    if (frameBytes == 0)
        return kQueueMax;
    return std::clamp(kQueueBudgetBytes / frameBytes, kQueueMin, kQueueMax);
}

// Seek the next clip's source this long (timeline) before its boundary, on a
// worker: a seek decodes from the keyframe up to the trim-in, ~0.3 s for an
// 8K clip, which the decoder can't do in time while the queue holds ~0.2 s.
constexpr qint64 kBoundaryPrefetchMs = 1500;

// The next frame of `src`: the lens pair if `useLens`, else the stitched
// equirect. False at the end of the source or on an error.
bool readSourceFrame(stitch::IStitchSource &src, bool useLens, stitch::Frame &frame,
                     std::shared_ptr<render::LensFrame> &lens)
{
    if (useLens) {
        lens = std::make_shared<render::LensFrame>();
        if (src.ReadLensFrame(*lens)) {
            frame.timestamp_ms = lens->timestamp_ms;
            return true;
        }
        lens.reset();
        return false;
    }
    frame = src.ReadFrame(); // the slow part
    return !frame.rgba.empty();
}
} // namespace

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
{
}

PlaybackController::~PlaybackController()
{
    stopDecoder();
}

// ---------------------------------------------------------------------------
// Timeline math
// ---------------------------------------------------------------------------

QList<PlaybackController::Placed> PlaybackController::placeSegments(const QList<Segment> &segments)
{
    QList<Placed> placed;
    qint64 acc = 0;
    for (const Segment &s : segments) {
        Placed p;
        p.seg = s;
        p.startMs = acc;
        acc += p.durationMs();
        placed.push_back(p);
    }
    return placed;
}

int PlaybackController::segmentAt(const QList<Placed> &placed, qint64 timelineMs)
{
    for (int i = 0; i < placed.size(); ++i) {
        if (timelineMs < placed[i].endMs())
            return i;
    }
    return placed.size() - 1; // past the end, or -1 if empty
}

// ---------------------------------------------------------------------------
// GUI-thread entry points
// ---------------------------------------------------------------------------

void PlaybackController::setTimeline(const QList<Segment> &segments)
{
    QList<Placed> placed = placeSegments(segments);
    m_totalMs = placed.isEmpty() ? 0 : placed.back().endMs();
    m_haveTimeline = !placed.isEmpty();
    m_positionMs = clampTimeline(m_positionMs);
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_presentAfterSeek = true;
    m_reachedEnd = false;

    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_placed = std::move(placed);
        m_timelineDirty = true;
        m_decoderEof = false;
        requestSeekLocked(m_positionMs);
    }
    m_cv.notify_all();

    if (m_haveTimeline) {
        if (!m_timer) {
            m_timer = new QTimer(this);
            m_timer->setTimerType(Qt::PreciseTimer);
            connect(m_timer, &QTimer::timeout, this, &PlaybackController::tick);
        }
        if (!m_timer->isActive())
            m_timer->start(kPresentIntervalMs);
        startDecoder();
    }

    emit durationChanged(m_totalMs);
    emit positionChanged(m_positionMs);
}

void PlaybackController::setPreviewConfig(const stitch::PreviewStitchConfig &config)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (config == m_pendingPreviewConfig)
            return;
        m_pendingPreviewConfig = config;
        m_previewConfigDirty = true;
        m_decoderEof = false;
        // Re-decode from where we are so the current frame re-stitches under
        // the new settings. requestSeekLocked bumps m_seekGen and sets
        // m_hardStall via the decoder, so the presenter freezes the playhead
        // through the re-stitch instead of running the clock on.
        if (m_haveTimeline)
            requestSeekLocked(m_positionMs);
    }
    // While paused the presenter only picks up a frame after a seek; without
    // this the re-stitched frame was decoded but never shown until the next
    // play/scrub.
    if (m_haveTimeline)
        m_presentAfterSeek = true;
    m_cv.notify_all();
}

void PlaybackController::play()
{
    if (!m_haveTimeline || m_playing)
        return;
    if (m_positionMs >= m_totalMs - kEndEpsilonMs) {
        m_positionMs = 0;
        std::lock_guard<std::mutex> lk(m_mx);
        requestSeekLocked(0);
    }
    m_reachedEnd = false;
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_prerolling = true; // start on a cushion of frames -- see kResumeFrames
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_playing = true;
    }
    m_cv.notify_all();
    emit playingChanged(true);
}

void PlaybackController::pause()
{
    if (!m_playing)
        return;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_playing = false;
    }
    m_positionMs = clampTimeline(m_clockBaseMs + m_clock.elapsed());
    emit playingChanged(false);
    emit positionChanged(m_positionMs);
}

void PlaybackController::togglePlay()
{
    if (m_playing)
        pause();
    else
        play();
}

void PlaybackController::seek(qint64 timelineMs)
{
    if (!m_haveTimeline)
        return;
    m_positionMs = clampTimeline(timelineMs);
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_reachedEnd = false;
    // Frozen from here until kResumeFrames of the new position are queued --
    // not just until the first one is read (m_hardStall): resuming on a
    // single frame ran the queue dry again at once.
    m_prerolling = true;
    if (m_settleTimer)
        m_settleTimer->stop();
    {
        std::lock_guard<std::mutex> lk(m_mx);
        const Handy h = frameAtLocked(m_positionMs);
        if (h == Handy::No)
            requestSeekLocked(m_positionMs);
        m_presentAfterSeek = h != Handy::OnScreen;
    }
    m_cv.notify_all();
    emit positionChanged(m_positionMs);
    if (m_resumeAfterScrub) { // the end of a drag that started during playback
        m_resumeAfterScrub = false;
        play();
    }
}

void PlaybackController::scrub(qint64 timelineMs)
{
    if (!m_haveTimeline)
        return;
    // Dragging during playback scrubs exactly like paused: pause for the drag,
    // and the drag's final seek() resumes.
    if (m_playing) {
        pause();
        m_resumeAfterScrub = true;
    }
    m_positionMs = clampTimeline(timelineMs);
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_reachedEnd = false;
    m_prerolling = true;
    Handy h;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        h = frameAtLocked(m_positionMs);
        if (h == Handy::No)
            requestSeekLocked(m_positionMs, /*keyframe=*/true);
        m_presentAfterSeek = h != Handy::OnScreen;
    }
    m_cv.notify_all();
    emit positionChanged(m_positionMs);
    // The decoder may land on a keyframe; once the pointer rests, settle()
    // brings the exact frame.
    if (h == Handy::No) {
        if (!m_settleTimer) {
            m_settleTimer = new QTimer(this);
            m_settleTimer->setSingleShot(true);
            m_settleTimer->setInterval(kScrubSettleMs);
            connect(m_settleTimer, &QTimer::timeout, this, &PlaybackController::settle);
        }
        m_settleTimer->start();
    }
}

void PlaybackController::settle()
{
    if (!m_haveTimeline || m_playing)
        return;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        const Handy h = frameAtLocked(m_positionMs);
        if (h == Handy::No)
            requestSeekLocked(m_positionMs);
        m_presentAfterSeek = h != Handy::OnScreen;
    }
    m_cv.notify_all();
}

PlaybackController::Handy PlaybackController::frameAtLocked(qint64 timelineMs)
{
    // Only while paused, with the shown frame and the queue both from the
    // latest seek: then the queue is the decoder's lookahead, in order, right
    // after the shown frame.
    if (m_playing || m_seekPending || m_shownGen != m_seekGen)
        return Handy::No;
    const qint64 limit = timelineMs + kSameFrameMs; // a frame starting by here
    size_t n = 0;
    while (n < m_queue.size() && m_queue[n].frame.timestamp_ms <= limit)
        ++n;
    if (n == 0) {
        // Before every queued frame: the shown one, if it starts by then --
        // and with nothing queued only if it starts right there.
        const bool shown = m_queue.empty()
                               ? std::abs(m_shownMs - timelineMs) <= kSameFrameMs
                               : m_shownMs <= limit && timelineMs >= m_shownMs - kSameFrameMs;
        return shown ? Handy::OnScreen : Handy::No;
    }
    // Queued frame n-1 is the one, if a later one is queued or it starts
    // right there (no telling where the last one ends).
    if (n == m_queue.size() &&
        std::abs(m_queue[n - 1].frame.timestamp_ms - timelineMs) > kSameFrameMs)
        return Handy::No;
    m_queue.erase(m_queue.begin(), m_queue.begin() + (n - 1));
    return Handy::Queued;
}

void PlaybackController::requestSeekLocked(qint64 timelineMs, bool keyframe)
{
    m_seekPending = true;
    m_seekTargetMs = timelineMs;
    m_seekToKeyframe = keyframe;
    m_decoderEof = false;
    ++m_seekGen;
    // While playing, drop buffered frames so the queue re-forms cleanly around
    // the new clock position. A paused exact seek drops them too -- they're
    // from before it and must never be shown; the viewport keeps its
    // picture until the target arrives. While scrubbing keep an earlier
    // scrub's target not shown yet: a fast drag fires seeks faster than one
    // decode, and clearing every one would freeze the picture for the whole
    // drag (tick()). The rest is lookahead nobody will show; it would only
    // fill the queue and stall the decoder.
    if (m_playing || !keyframe)
        m_queue.clear();
    else
        std::erase_if(m_queue, [this](const QueuedFrame &q) { return q.gen <= m_shownGen; });
}

// ---------------------------------------------------------------------------
// Decoder thread
// ---------------------------------------------------------------------------

void PlaybackController::startDecoder()
{
    if (m_decoder.joinable())
        return;
    m_decoder = std::thread([this] { decoderLoop(); });
}

void PlaybackController::stopDecoder()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_decoder.joinable())
        m_decoder.join();
    m_prefetch.reset(); // waits for a job still running
    m_heldRead.reset();
}

void PlaybackController::touchSourceLru(const QString &path)
{
    m_srcLru.removeAll(path);
    m_srcLru.append(path); // most-recently-used at the back
    // Evict the coldest while over budget, never the active source.
    while (m_srcCache.size() > kMaxCachedSources && !m_srcLru.isEmpty()) {
        const QString victim = m_srcLru.front();
        if (victim == m_activePath) {
            if (m_srcLru.size() <= 1)
                break;
            m_srcLru.move(0, m_srcLru.size() - 1);
            continue;
        }
        m_srcLru.removeFirst();
        m_srcCache.erase(victim);
    }
}

std::shared_ptr<stitch::IStitchSource> PlaybackController::acquireSource(const QString &path,
                                                                         QString *err)
{
    auto it = m_srcCache.find(path);
    if (it != m_srcCache.end()) {
        touchSourceLru(path);
        return it->second;
    }

    std::shared_ptr<stitch::IStitchSource> src = stitch::MakeStitchSource();
    if (!src->Open(path.toStdString(), m_decPreviewConfig)) {
        if (err)
            *err = QString::fromStdString(src->GetLastError());
        return nullptr;
    }
    m_srcCache.emplace(path, src);
    touchSourceLru(path);
    return src;
}

void PlaybackController::setCaptureTimings(std::map<int, stitch::CaptureTiming> timings)
{
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        m_rsSettings = std::move(timings);
    }
    m_rsGen.fetch_add(1);
}

void PlaybackController::setSeamAdjust(const QString &path, std::array<double, 3> deg)
{
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        m_seamAdjust[path] = deg;
    }
    m_rsGen.fetch_add(1);
}

void PlaybackController::setBlurQuads(const QString &path, stitch::BlurQuadsFn fn)
{
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        if (fn)
            m_blurQuads[path] = std::move(fn);
        else
            m_blurQuads.erase(path);
    }
    m_rsGen.fetch_add(1);
}

void PlaybackController::setStabilizationProvider(StabProvider provider)
{
    std::lock_guard<std::mutex> lk(m_stabMx);
    m_stabProvider = std::move(provider);
}

void PlaybackController::setOutlineProvider(OutlineProvider provider)
{
    std::lock_guard<std::mutex> lk(m_stabMx);
    m_outlineProvider = std::move(provider);
}

void PlaybackController::applyCaptureTimingIfChanged()
{
    if (!m_activeSource)
        return;
    const uint64_t gen = m_rsGen.load();
    const int clipId = m_decPlaced[m_decSegIndex].seg.clipId;
    const ClipHandOver handOver = handOverAfter(m_decSegIndex);
    if (gen == m_decRsGen && m_activePath == m_decRsPath && clipId == m_decRsClipId &&
        handOver.untilMs == m_decRsUntilMs && handOver.afterClipId == m_decRsAfterClipId)
        return;
    applySourceSettings(*m_activeSource, m_activePath, clipId, handOver);
    m_decRsGen = gen;
    m_decRsPath = m_activePath;
    m_decRsClipId = clipId;
    m_decRsUntilMs = handOver.untilMs;
    m_decRsAfterClipId = handOver.afterClipId;
}

PlaybackController::ClipHandOver PlaybackController::handOverAfter(int index) const
{
    // Any frame from the cut on read in this segment either carries on into
    // the next one or is dropped for a seek, so the hand-over can sit at
    // the trim-out whether or not the decoder ends up carrying on.
    ClipHandOver h;
    if (index < 0 || index + 1 >= m_decPlaced.size())
        return h;
    const Segment &cur = m_decPlaced[index].seg;
    const Segment &next = m_decPlaced[index + 1].seg;
    if (next.sourcePath != cur.sourcePath)
        return h;
    h.untilMs = cur.outMs;
    h.afterClipId = next.clipId;
    return h;
}

void PlaybackController::applySourceSettings(stitch::IStitchSource &src, const QString &path,
                                             int clipId, ClipHandOver handOver)
{
    RsSetting s;
    RsSetting after;
    std::array<double, 3> seam{0, 0, 0};
    stitch::BlurQuadsFn blur;
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        auto it = m_rsSettings.find(clipId);
        if (it != m_rsSettings.end())
            s = it->second;
        auto at = m_rsSettings.find(handOver.afterClipId);
        if (handOver.afterClipId && at != m_rsSettings.end())
            after = at->second;
        auto st = m_seamAdjust.find(path);
        if (st != m_seamAdjust.end())
            seam = st->second;
        auto bt = m_blurQuads.find(path);
        if (bt != m_blurQuads.end())
            blur = bt->second;
    }
    src.SetCaptureTiming(std::move(s), handOver.untilMs, std::move(after));
    src.SetLens1Adjust(seam.data());
    src.SetBlurQuads(std::move(blur));
}

void PlaybackController::setHardStall(bool on)
{
    std::lock_guard<std::mutex> lk(m_mx);
    m_hardStall = on;
}

void PlaybackController::pruneSourcesToTimeline()
{
    for (auto it = m_srcCache.begin(); it != m_srcCache.end();) {
        const bool referenced =
            std::any_of(m_decPlaced.begin(), m_decPlaced.end(),
                        [&](const Placed &p) { return p.seg.sourcePath == it->first; });
        if (referenced) {
            ++it;
        } else {
            m_srcLru.removeAll(it->first);
            it = m_srcCache.erase(it);
        }
    }
}

bool PlaybackController::switchToSegment(int index)
{
    const QString path = m_decPlaced[index].seg.sourcePath;
    if (m_activeSource && m_activePath == path) {
        touchSourceLru(path);
        return true;
    }

    QString err;
    auto src = acquireSource(path, &err);
    if (!src) {
        emit openFailed(err.isEmpty() ? QStringLiteral("could not open %1").arg(path) : err);
        return false;
    }
    // The source we leave keeps its lens ring only for frames still queued or
    // on screen; drop the rest of its GPU memory.
    if (m_activeSource)
        m_activeSource->TrimLensFrames();
    m_activeSource = std::move(src);
    m_activePath = path;
    return true;
}

void PlaybackController::startPrefetch(const Placed &cur, qint64 timelineMs)
{
    if (m_prefetch) {
        if (!m_prefetch->stale)
            return; // already under way for this boundary
        if (m_prefetch->done.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return; // one job at a time
        m_prefetch.reset();
    }
    const int next = m_decSegIndex + 1;
    if (next >= m_decPlaced.size() || cur.endMs() - timelineMs > kBoundaryPrefetchMs)
        return;
    const Placed &np = m_decPlaced[next];
    // The same file on from here (a split): the decoder carries on reading.
    if (np.seg.sourcePath == cur.seg.sourcePath && np.seg.inMs == cur.seg.outMs)
        return;

    auto p = std::make_unique<Prefetch>();
    p->segIndex = next;
    p->path = np.seg.sourcePath;
    // A cached source other than the active one is the job's until taken; a
    // jump within the active file needs a second one (opened by the job).
    if (p->path != m_activePath) {
        auto it = m_srcCache.find(p->path);
        if (it != m_srcCache.end()) {
            p->src = std::move(it->second);
            m_srcCache.erase(it);
            m_srcLru.removeAll(p->path);
        }
    }
    const int clipId = np.seg.clipId;
    const ClipHandOver handOver = handOverAfter(next);
    if (p->src)
        applySourceSettings(*p->src, p->path, clipId, handOver);
    const bool viewportLens = m_viewport && m_viewport->LensFramesSupported();
    Prefetch *job = p.get();
    p->done = std::async(std::launch::async,
                         [this, job, viewportLens, clipId, handOver, inMs = np.seg.inMs,
                          config = m_decPreviewConfig] {
                             if (!job->src) {
                                 auto src = stitch::MakeStitchSource();
                                 if (!src->Open(job->path.toStdString(), config))
                                     return; // the boundary opens it again and reports why
                                 applySourceSettings(*src, job->path, clipId, handOver);
                                 job->src = std::move(src);
                             }
                             job->src->SeekTo(inMs);
                             job->got = readSourceFrame(*job->src,
                                                        viewportLens && job->src->LensFramesAvailable(),
                                                        job->frame, job->lens);
                             job->opened = true;
                         });
    m_prefetch = std::move(p);
}

bool PlaybackController::takePrefetch(int next)
{
    if (!m_prefetch || m_prefetch->stale || m_prefetch->segIndex != next)
        return false;
    setHardStall(true);      // cleared by the next read, as after a SeekTo()
    m_prefetch->done.wait(); // normally done long ago
    std::unique_ptr<Prefetch> p = std::move(m_prefetch);
    if (!p->opened)
        return false;
    if (m_activeSource)
        m_activeSource->TrimLensFrames(); // as switchToSegment
    m_srcCache[p->path] = p->src; // a jump within the file replaces the active one
    m_activeSource = std::move(p->src);
    m_activePath = p->path;
    touchSourceLru(m_activePath);
    m_decRsGen = ~uint64_t(0); // re-apply: settings may have changed since
    m_heldRead = HeldRead{std::move(p->frame), std::move(p->lens), p->got};
    return true;
}

void PlaybackController::invalidatePrefetch()
{
    if (m_prefetch)
        m_prefetch->stale = true;
    m_heldRead.reset();
}

void PlaybackController::decoderLoop()
{
    for (;;) {
        bool doSeek = false;
        bool tlChanged = false;
        bool cfgChanged = false;
        qint64 seekMs = 0;
        bool seekToKeyframe = false;
        uint64_t gen = 0;
        bool playing = false;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this] {
                const size_t cap = !m_playing   ? kScrubLookahead
                                   : m_lensQueue ? kLensQueueMax
                                                 : queueCapFor(m_frameBytes);
                const bool canDecode =
                    !m_decPlaced.isEmpty() && !m_decoderEof && m_queue.size() < cap;
                return m_stop || m_seekPending || m_timelineDirty || m_previewConfigDirty ||
                       canDecode;
            });
            if (m_stop)
                return;
            if (m_timelineDirty) {
                m_decPlaced = m_placed;
                m_timelineDirty = false;
                tlChanged = true;
            }
            if (m_previewConfigDirty) {
                m_decPreviewConfig = m_pendingPreviewConfig;
                m_previewConfigDirty = false;
                cfgChanged = true;
            }
            if (m_seekPending) {
                m_seekPending = false;
                doSeek = true;
                seekMs = m_seekTargetMs;
                seekToKeyframe = m_seekToKeyframe;
                m_hardStall = true; // hold the presenter clock across the seek
            }
            gen = m_seekGen;
            playing = m_playing;
        }
        if (tlChanged || cfgChanged || doSeek)
            invalidatePrefetch();

        if (tlChanged) {
            pruneSourcesToTimeline(); // drop sources no longer on the timeline
            if (m_decPlaced.isEmpty()) {
                m_activeSource.reset();
                m_activePath.clear();
                m_decSegIndex = -1;
            }
        }

        if (cfgChanged) {
            // Every cached source was opened with the old settings. Drop them
            // all so the next switchToSegment re-opens under the new config;
            // the pending seek (from setPreviewConfig) then re-stitches the
            // current frame.
            m_srcCache.clear();
            m_srcLru.clear();
            m_activeSource.reset();
            m_activePath.clear();
            m_decSegIndex = -1;
        }

        if (m_decPlaced.isEmpty())
            continue; // nothing to play; wait for the next timeline/seek/stop

        if (doSeek) {
            const int idx = segmentAt(m_decPlaced, seekMs);
            const Placed &ps = m_decPlaced[idx];
            const qint64 clamped = std::clamp<qint64>(seekMs, ps.startMs, ps.endMs());
            if (!switchToSegment(idx)) {
                setHardStall(false);
                std::lock_guard<std::mutex> lk(m_mx);
                m_decoderEof = true;
                continue;
            }
            // At most the clip's last frame: a seek to its end (or to the last
            // frame's own time, a hair past it once rounded) decoded the frame
            // after the trim-out, read as the end -- and the old picture
            // stayed on screen.
            const double fps = m_activeSource->GetInfo().fps;
            const qint64 frameMs = std::llround(1000.0 / fps);
            const qint64 sourceMs = std::max(
                ps.seg.inMs, std::min(ps.seg.sourceAtLocalMs(clamped - ps.startMs), ps.seg.outMs - frameMs));
            qint64 target = sourceMs;
            if (seekToKeyframe) {
                // Scrubbing: the exact frame when it decodes about as fast as a
                // keyframe -- a little after its own keyframe, or a little
                // ahead of the decoder (the source's SeekTo decodes forward to
                // it). Else a keyframe on the way from where the decoder is,
                // which decodes in one step: a stop on the way, never past the
                // target, so the picture only ever moves the way the pointer
                // went. With none on the way, the exact frame after all --
                // scrubs arriving meanwhile merge into the next one. Keyframes
                // only inside this clip's trim.
                int64_t before = -1, after = -1;
                m_activeSource->KeyframesAround(sourceMs, before, after);
                const bool sameSeg = idx == m_decSegIndex && m_decLastSourceMs >= 0;
                const qint64 at = m_decLastSourceMs; // ~the frame on screen + lookahead
                const bool cheap =
                    (before >= 0 && sourceMs - before <= kScrubDecodeAheadMs) ||
                    (sameSeg && sourceMs > at && sourceMs - at <= kScrubDecodeAheadMs);
                qint64 kf = -1;
                if (!cheap && !sameSeg) // nothing on screen to keep in step with
                    kf = m_activeSource->NearestKeyframeMs(sourceMs);
                else if (!cheap && sourceMs > at && before > at)
                    kf = before;
                else if (!cheap && sourceMs < at && after >= 0 && after < at - kScrubHoldMs)
                    kf = after;
                if (kf >= ps.seg.inMs && kf < ps.seg.outMs)
                    target = kf;
            }
            m_activeSource->SeekTo(target); // m_hardStall stays until the first read
            m_decLastSourceMs = -1;
            m_decSegIndex = idx;
            std::lock_guard<std::mutex> lk(m_mx);
            m_decoderEof = false;
            continue;
        }

        if (m_decSegIndex < 0 || m_decSegIndex >= m_decPlaced.size()) {
            // No valid position yet -- seek to the current clamped target.
            std::lock_guard<std::mutex> lk(m_mx);
            m_seekPending = true;
            m_seekTargetMs = seekMs;
            continue;
        }

        Placed cur = m_decPlaced[m_decSegIndex]; // small copy; released lock

        applyCaptureTimingIfChanged();
        // Lens frames when the viewport can take them: the decoded pair, drawn
        // from the lenses at present time. Else the stitched equirect.
        const bool useLens = m_viewport && m_viewport->LensFramesSupported() &&
                             m_activeSource->LensFramesAvailable();
        const auto t0 = std::chrono::steady_clock::now();
        stitch::Frame frame;
        std::shared_ptr<render::LensFrame> lens;
        bool got = false;
        if (m_heldRead) {
            frame = std::move(m_heldRead->frame);
            lens = std::move(m_heldRead->lens);
            got = m_heldRead->got;
            m_heldRead.reset();
        } else {
            got = readSourceFrame(*m_activeSource, useLens, frame, lens);
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (render::framestats::Enabled() && got) {
            std::lock_guard<std::mutex> lk(m_mx);
            m_statsReadMs.add(render::framestats::MsBetween(t0, t1));
        }
        // SeekTo() only positions the decoder; this first read after it does
        // the decoding up to the target, the slow part of a seek or boundary.
        setHardStall(false);

        const bool sourceEnded = !got;
        const bool pastTrimOut = !sourceEnded && frame.timestamp_ms >= cur.seg.outMs;
        if (sourceEnded || pastTrimOut) {
            const int next = m_decSegIndex + 1;
            if (next >= m_decPlaced.size()) {
                std::lock_guard<std::mutex> lk(m_mx);
                m_decoderEof = true;
                continue;
            }
            // The next clip carries on in this file from this very frame (a
            // split): it's the frame SeekTo(trim-in) would give, so skip the
            // seek and its decode. The source was told to hand over to this
            // clip's capture timing at the cut (handOverAfter), so the frame
            // was stitched with the right one.
            const Placed &np = m_decPlaced[next];
            const bool carriesOn = pastTrimOut && np.seg.sourcePath == m_activePath &&
                                   m_decLastSourceMs >= 0 && m_decLastSourceMs < np.seg.inMs &&
                                   np.seg.inMs <= frame.timestamp_ms &&
                                   frame.timestamp_ms < np.seg.outMs;
            if (carriesOn) {
                m_decSegIndex = next;
                cur = np;
            } else if (takePrefetch(next)) {
                // Its first frame is read already: the next read returns it.
                m_decLastSourceMs = -1;
                m_decSegIndex = next;
                continue;
            } else if (!switchToSegment(next)) {
                std::lock_guard<std::mutex> lk(m_mx);
                m_decoderEof = true;
                continue;
            } else {
                // Until the next clip's first frame is read: its seek and
                // decode up to the trim-in.
                setHardStall(true);
                m_activeSource->SeekTo(m_decPlaced[next].seg.inMs);
                m_decLastSourceMs = -1;
                m_decSegIndex = next;
                continue;
            }
        }

        // Rewrite the frame's timestamp from source time to timeline time.
        const qint64 sourceMs = frame.timestamp_ms;
        m_decLastSourceMs = sourceMs;
        frame.timestamp_ms = cur.startMs + cur.seg.localAtSourceMs(frame.timestamp_ms);
        if (playing)
            startPrefetch(cur, frame.timestamp_ms);

        {
            std::lock_guard<std::mutex> lk(m_mx);
            // A stale frame from before a seek: dropped unless a scrub is in
            // flight, where any recent picture beats a frozen one.
            if (m_seekGen != gen && (m_playing || !m_seekToKeyframe))
                continue;
            if (!m_playing) {
                while (m_queue.size() >= kScrubLookahead)
                    m_queue.pop_front();
            }
            m_lensQueue = lens != nullptr;
            if (!lens)
                m_frameBytes = frame.rgba.size();
            m_queue.push_back({std::move(frame), std::move(lens), gen, cur.seg.sourcePath, sourceMs,
                               cur.seg.clipId});
        }
    }
}

// ---------------------------------------------------------------------------
// Present tick (controller's QThread)
// ---------------------------------------------------------------------------

void PlaybackController::setClockHeld(bool held)
{
    if (held != m_clockHeld) {
        m_clockHeld = held;
        emit clockHeldChanged(held);
    }
}

void PlaybackController::presentInsets(qint64 timelineMs, bool playing)
{
    std::vector<EquirectViewportWindow::InsetDraw> draws = m_insets.frameFor(timelineMs, playing);
    auto same = [](const EquirectViewportWindow::InsetDraw &a,
                   const EquirectViewportWindow::InsetDraw &b) {
        return a.clipId == b.clipId && a.frameKey == b.frameKey && a.look == b.look &&
               a.frame == b.frame && a.aspect == b.aspect;
    };
    if (draws.size() == m_lastInsets.size() &&
        std::equal(draws.begin(), draws.end(), m_lastInsets.begin(), same))
        return;
    m_lastInsets = draws;
    if (m_viewport)
        m_viewport->SetInsets(std::move(draws));
}

void PlaybackController::statsTick(bool playing, size_t queued)
{
    Stats &s = m_stats;
    const auto now = render::framestats::Clock::now();
    if (!playing) { // only playback is summarised; start afresh on the next play
        s = Stats{};
        return;
    }
    if (s.windowStart == render::framestats::Clock::time_point{})
        s.windowStart = now;
    s.minQueue = std::min(s.minQueue, queued);
    if (render::framestats::MsBetween(s.windowStart, now) < 1000.0)
        return;
    render::framestats::Samples read;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        std::swap(read, m_statsReadMs);
    }
    std::fprintf(stderr,
                 "[stats] playback @%.1fs: shown %d, skipped %d, buffering ticks %d, min queue "
                 "%zu | late mean %.1f max %.1f ms | decoder read n %zu mean %.2f p99 %.2f max "
                 "%.2f ms | stab lookup n %zu mean %.3f max %.3f ms\n",
                 m_positionMs / 1000.0, s.shown, s.skipped, s.bufferingTicks,
                 s.minQueue == SIZE_MAX ? size_t(0) : s.minQueue, s.lateMs.mean(), s.lateMs.max(),
                 read.count(), read.mean(), read.pct(0.99), read.max(), s.stabMs.count(),
                 s.stabMs.mean(), s.stabMs.max());
    s = Stats{};
    s.windowStart = now;
}

void PlaybackController::tick()
{
    if (!m_haveTimeline)
        return;

    const bool playing = m_playing;

    bool hardStall = false;
    bool seekPending = false;
    size_t queued = 0;
    size_t resumeFrames = kResumeFrames; // never more than the queue can hold
    bool decoderEof = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        hardStall = m_hardStall;
        seekPending = m_seekPending;
        queued = m_queue.size();
        resumeFrames = std::min(kResumeFrames,
                                m_lensQueue ? kLensQueueMax : queueCapFor(m_frameBytes));
        decoderEof = m_decoderEof;
    }

    // A stitch is blocking the decoder (a seek, or opening the next clip at a
    // boundary) and we've shown everything we had. Freeze the playhead and show
    // "Buffering…"; re-anchor so the clock resumes from here instead of
    // counting the stall as elapsed playback. Stay frozen until kResumeFrames
    // are queued, so playback resumes smoothly.
    if (playing) {
        const bool stalled = (hardStall || seekPending) && queued == 0;
        if (stalled)
            m_prerolling = true;
        if (m_prerolling) {
            if (stalled || (queued < resumeFrames && !decoderEof)) {
                if (render::framestats::Enabled()) {
                    ++m_stats.bufferingTicks;
                    statsTick(true, queued);
                }
                m_clockBaseMs = m_positionMs;
                m_clock.restart();
                if (!m_buffering) {
                    m_buffering = true;
                    emit bufferingChanged(true);
                }
                setClockHeld(true);
                presentInsets(m_positionMs, false); // frozen with the picture
                return;
            }
            m_prerolling = false;
        }
        setClockHeld(false);
    }

    qint64 clockMs = m_positionMs;
    if (playing) {
        clockMs = std::clamp<qint64>(m_clockBaseMs + m_clock.elapsed(), 0, m_totalMs);
    }

    stitch::Frame chosen;
    std::shared_ptr<const render::LensFrame> chosenLens;
    QString chosenPath;
    qint64 chosenSourceMs = 0;
    int chosenClipId = 0;
    bool haveChosen = false;
    bool chosenIsCurrent = false;
    bool queueEmpty = true;
    bool eof = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (playing) {
            while (!m_queue.empty() && m_queue.front().frame.timestamp_ms <= clockMs) {
                if (haveChosen)
                    ++m_stats.skipped; // replaced before it was ever shown
                chosen = std::move(m_queue.front().frame);
                chosenLens = std::move(m_queue.front().lens);
                chosenPath = m_queue.front().sourcePath;
                chosenSourceMs = m_queue.front().sourceMs;
                chosenClipId = m_queue.front().clipId;
                m_shownGen = m_queue.front().gen;
                m_shownMs = m_queue.front().frame.timestamp_ms;
                m_queue.pop_front();
                haveChosen = true;
            }
        } else if (m_presentAfterSeek && !m_queue.empty()) {
            // The seek's own frame is the first of its generation; any after
            // it are lookahead, kept for a step forward (seek()). Without one
            // yet, a scrub may show an earlier scrub's target that never
            // reached the screen -- a fast drag outruns the decoder
            // -- but never older lookahead, or anything from
            // before an exact seek. The queue is in generation order.
            const uint64_t gen = m_queue.back().gen;
            chosenIsCurrent = gen == m_seekGen;
            if (chosenIsCurrent || (m_seekToKeyframe && gen > m_shownGen)) {
                auto it = std::find_if(m_queue.begin(), m_queue.end(),
                                       [gen](const QueuedFrame &q) { return q.gen == gen; });
                chosen = std::move(it->frame);
                chosenLens = std::move(it->lens);
                chosenPath = it->sourcePath;
                chosenSourceMs = it->sourceMs;
                chosenClipId = it->clipId;
                m_queue.erase(m_queue.begin(), std::next(it));
                m_shownGen = gen;
                m_shownMs = chosen.timestamp_ms;
                haveChosen = true;
            }
        }
        queueEmpty = m_queue.empty();
        eof = m_decoderEof;
    }
    if (haveChosen)
        m_cv.notify_all(); // room in the queue -- let the decoder run

    if (haveChosen) {
        m_presentAfterSeek = !playing && !chosenIsCurrent;
        if (m_viewport) {
            StabProvider provider;
            OutlineProvider outlineProvider;
            {
                std::lock_guard<std::mutex> lk(m_stabMx);
                provider = m_stabProvider;
                outlineProvider = m_outlineProvider;
            }
            float stab[9];
            const auto stabT0 = render::framestats::Clock::now();
            const bool haveStab = provider && provider(chosenClipId, chosenSourceMs, stab);
            if (render::framestats::Enabled() && playing) {
                ++m_stats.shown;
                m_stats.lateMs.add(double(clockMs - chosen.timestamp_ms));
                if (haveStab)
                    m_stats.stabMs.add(
                        render::framestats::MsBetween(stabT0, render::framestats::Clock::now()));
            }
            std::vector<EquirectViewportWindow::RegionOutline> outlines;
            if (outlineProvider)
                outlineProvider(chosenPath, chosenSourceMs, outlines);
            if (chosenLens)
                m_viewport->SetLensFrame(std::move(chosenLens), haveStab ? stab : nullptr,
                                         std::move(outlines));
            else
                m_viewport->SetFrame(chosen.width, chosen.height, std::move(chosen.rgba),
                                     haveStab ? stab : nullptr, std::move(outlines));
        }
    }

    presentInsets(clockMs, playing);
    if (render::framestats::Enabled())
        statsTick(playing, queued);

    const bool wantFrame = playing || m_presentAfterSeek;
    const bool starved = wantFrame && !haveChosen && queueEmpty && !eof;
    if (starved != m_buffering) {
        m_buffering = starved;
        emit bufferingChanged(m_buffering);
    }

    if (playing) {
        m_positionMs = clockMs;
        emit positionChanged(m_positionMs);
        const bool clockAtEnd = clockMs >= m_totalMs - kEndEpsilonMs;
        if ((clockAtEnd || (queueEmpty && eof)) && !m_reachedEnd) {
            m_reachedEnd = true;
            m_positionMs = m_totalMs;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                m_playing = false;
                m_queue.clear();
            }
            if (m_buffering) {
                m_buffering = false;
                emit bufferingChanged(false);
            }
            setClockHeld(false);
            emit playingChanged(false);
            emit positionChanged(m_positionMs);
        }
    }
}
