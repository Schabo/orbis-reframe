#include "PlaybackController.h"
#include "BlankClipSource.h"
#include "PlaybackTuning.h"

#include "../render/EquirectViewportWindow.h"

#include <NativeStitchSource.h> // stitch::MakeStitchSource
#include <VulkanHwDevice.h>     // core::sharedDeviceMemory
#include <Log.h>

#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

using namespace playbacktuning;

namespace {

// The next frame of `src`: the lens pair if `useLens`, else the stitched
// equirect. False at the end of the source or on an error.
bool readSourceFrame(stitch::IStitchSource &src, bool useLens, stitch::Frame &frame,
                     std::shared_ptr<render::LensFrame> &lens)
{
    if (useLens) {
        lens = std::make_shared<render::LensFrame>();
        if (src.ReadLensFrame(*lens)) {
            frame.timestamp_ms = lens->timestamp_ms;
            frame.timestamp_us = lens->timestamp_us;
            return true;
        }
        lens.reset();
        return false;
    }
    frame = src.ReadFrame(); // the slow part
    return !frame.rgba.empty();
}

constexpr uint64_t kMiB = 1024 * 1024;

using core::gpuMargin;

double toMiB(uint64_t bytes)
{
    return double(bytes) / double(kMiB);
}

} // namespace

// ---------------------------------------------------------------------------
// Graphics memory
// ---------------------------------------------------------------------------

uint64_t PlaybackController::sourceGpuNeed(const QString &path) const
{
    const auto it = m_srcGpuBytes.find(path);
    if (it != m_srcGpuBytes.end())
        return it->second;
    return core::clipGpuBytes();
}

void PlaybackController::noteSourceGpuBytes(const QString &path, uint64_t bytes)
{
    // Measured as the process's usage before and after, so what else came
    // or went meanwhile (a measurement letting go, a job opening) is in it
    // too: the smallest figure seen for the file is the nearest.
    const auto [it, added] = m_srcGpuBytes.try_emplace(path, bytes);
    if (!added)
        it->second = std::min(it->second, bytes);
    uint64_t most = 0;
    for (const auto &[p, b] : m_srcGpuBytes)
        most = std::max(most, b);
    core::setClipGpuBytes(most);
}

bool PlaybackController::releaseSourceGpu(const QString &path, stitch::IStitchSource &src)
{
    const core::GpuMemory before = core::sharedDeviceMemory();
    if (!src.ReleaseGpu())
        return false;
    const core::GpuMemory after = core::sharedDeviceMemory();
    if (before.known && after.known && before.usage > after.usage)
        noteSourceGpuBytes(path, before.usage - after.usage);
    core::log::Info("[playback] gave back the graphics memory of %s (%.0f MiB; %.0f of %.0f MiB "
                    "free)\n",
                    qPrintable(path), toMiB(before.usage - std::min(before.usage, after.usage)),
                    toMiB(after.headroom()), toMiB(after.budget));
    return true;
}

bool PlaybackController::resumeSource(const QString &path, stitch::IStitchSource &src)
{
    if (!src.GpuReleased())
        return true;
    const core::GpuMemory before = core::sharedDeviceMemory();
    if (!src.ResumeGpu())
        return false;
    const core::GpuMemory after = core::sharedDeviceMemory();
    if (before.known && after.known && after.usage > before.usage)
        noteSourceGpuBytes(path, after.usage - before.usage);
    return true;
}

void PlaybackController::balanceGpuMemory(uint64_t need, bool includeActive, const QString &keep)
{
    m_lastGpuCheck = std::chrono::steady_clock::now();
    core::GpuMemory mem = core::sharedDeviceMemory();
    const auto roomy = [&] { return mem.known && mem.headroom() >= gpuMargin(mem) + need; };
    // Coldest first; releasing changes neither the cache nor the LRU.
    for (const QString &path : std::as_const(m_srcLru)) {
        if (roomy())
            break;
        if (path == m_activePath || path == keep)
            continue;
        const auto it = m_srcCache.find(path);
        if (it != m_srcCache.end() && releaseSourceGpu(path, *it->second))
            mem = core::sharedDeviceMemory();
    }
    if (!roomy() && m_lentOut && m_lentOut->path != keep) {
        // Lent to a crossfade's layer past its trim-out: playback's picture
        // comes first, the layer reads its own.
        const QString path = m_lentOut->path;
        takeBackLentOut(true);
        const auto it = m_srcCache.find(path);
        if (it != m_srcCache.end())
            releaseSourceGpu(path, *it->second);
        mem = core::sharedDeviceMemory(); // (or not cached again: closed)
    }
    if (includeActive && !roomy() && m_activeSource && m_activePath != keep &&
        releaseSourceGpu(m_activePath, *m_activeSource))
        mem = core::sharedDeviceMemory();
    if (!mem.known || roomy()) {
        m_gpuShortSaid = false;
        return;
    }
    if (!m_gpuShortSaid) {
        m_gpuShortSaid = true;
        std::fprintf(stderr,
                     "[playback] the graphics memory is short: %.0f MiB free of %.0f MiB, "
                     "%.0f MiB wanted; nothing more to give back\n",
                     toMiB(mem.headroom()), toMiB(mem.budget), toMiB(gpuMargin(mem) + need));
    }
}

void PlaybackController::retryGpuFallback()
{
    const auto now = std::chrono::steady_clock::now();
    if (!m_activeSource || !m_activeSource->GetGpuFallback().any() ||
        m_activeSource->GpuReleased() || now < m_gpuRetryAt || core::gpuJobRunning())
        return; // a job's room isn't for taking
    const core::GpuMemory mem = core::sharedDeviceMemory();
    if (!mem.known || mem.headroom() < gpuMargin(mem) + sourceGpuNeed(m_activePath))
        return;
    // Let go and take up again: the GPU tried afresh, the next read carries
    // on where the source was (a short stall). The budget can say there's
    // room when there isn't quite: tried again after a while, not at once.
    if (!m_activeSource->ReleaseGpu() || !resumeSource(m_activePath, *m_activeSource))
        return;
    const bool back = !m_activeSource->GetGpuFallback().any();
    m_gpuRetryAt = now + (back ? std::chrono::seconds(0) : std::chrono::seconds(30));
    core::log::Info("[playback] room on the graphics card again: %s %s\n", qPrintable(m_activePath),
                    back ? "is back on it" : "still doesn't fit; trying again in 30 s");
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
    invalidatePrefetch(); // a lent source back from its layer
    takeBackLentOut(true);
    m_prefetch.reset();   // waits for a job still running
    for (const std::shared_ptr<LayerLease> &lease : m_lender->takeAsks())
        lease->decline();
    for (const std::shared_ptr<LayerLease> &lease : std::exchange(m_layerAsksLater, {}))
        lease->decline();
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

std::unique_ptr<stitch::IStitchSource> PlaybackController::makeSource(const Segment &seg) const
{
    if (seg.flat)
        return std::make_unique<BlankClipSource>(seg.fps, seg.sourceDurationMs, true);
    return m_makeSource ? m_makeSource() : stitch::MakeStitchSource();
}

stitch::PreviewStitchConfig PlaybackController::openConfig(stitch::PreviewStitchConfig preview,
                                                           const Segment &seg)
{
    preview.lensGuard = seg.lensGuard;
    preview.lensCalibration = seg.lensCalibration;
    preview.method = seg.stitchMethod;
    preview.seamColourBlend = seg.seamColourBlend;
    preview.seamFlow = seg.seamFlow;
    return preview;
}

std::shared_ptr<stitch::IStitchSource> PlaybackController::acquireSource(const Segment &seg,
                                                                         QString *err)
{
    const QString &path = seg.sourcePath;
    if (m_lentOut && m_lentOut->path == path)
        takeBackLentOut(true); // wanted again (a seek back into it)
    auto it = m_srcCache.find(path);
    if (it != m_srcCache.end()) {
        touchSourceLru(path);
        if (it->second->GpuReleased()) {
            // Room for it first; the source being left may give its memory
            // back too (its frames still queued stay valid).
            balanceGpuMemory(sourceGpuNeed(path), true, path);
            if (!resumeSource(path, *it->second)) {
                if (err)
                    *err = QString::fromStdString(it->second->GetLastError());
                return nullptr;
            }
        }
        return it->second;
    }

    if (!seg.flat)
        balanceGpuMemory(sourceGpuNeed(path), true, path);
    std::shared_ptr<stitch::IStitchSource> src = makeSource(seg);
    const core::GpuMemory before = core::sharedDeviceMemory();
    if (!src->Open(path.toStdString(), openConfig(m_decPreviewConfig, seg))) {
        if (err)
            *err = QString::fromStdString(src->GetLastError());
        return nullptr;
    }
    const core::GpuMemory after = core::sharedDeviceMemory();
    if (!seg.flat && before.known && after.known && after.usage > before.usage)
        noteSourceGpuBytes(path, after.usage - before.usage);
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

void PlaybackController::setSeamCorrection(const QString &path, const insv::SeamCorrection &seam,
                                           const insv::LensColour &colour)
{
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        m_seamCorrection[path] = {seam, colour};
    }
    m_rsGen.fetch_add(1);
}

void PlaybackController::setBlurQuads(std::map<int, stitch::BlurQuadsFn> blurs)
{
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        m_blurQuads = std::move(blurs);
    }
    m_rsGen.fetch_add(1);
}

stitch::BlurQuadsFn PlaybackController::blurQuads(int clipId)
{
    std::lock_guard<std::mutex> lk(m_rsMx);
    const auto it = m_blurQuads.find(clipId);
    return it != m_blurQuads.end() ? it->second : stitch::BlurQuadsFn{};
}

void PlaybackController::setStabilizationProvider(StabProvider provider)
{
    std::lock_guard<std::mutex> lk(m_stabMx);
    m_stabProvider = std::move(provider);
}

bool PlaybackController::shownSourceUs(int clipId, qint64 &sourceUs) const
{
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_shownSourceUs < 0 || m_shownClipId != clipId)
        return false;
    sourceUs = m_shownSourceUs;
    return true;
}

void PlaybackController::setViewProvider(ViewProvider provider)
{
    std::lock_guard<std::mutex> lk(m_stabMx);
    m_viewProvider = std::move(provider);
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
    insv::SeamCorrection seam;
    insv::LensColour colour;
    stitch::BlurQuadsFn blur, blurAfter;
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        auto it = m_rsSettings.find(clipId);
        if (it != m_rsSettings.end())
            s = it->second;
        auto at = m_rsSettings.find(handOver.afterClipId);
        if (handOver.afterClipId && at != m_rsSettings.end())
            after = at->second;
        auto st = m_seamCorrection.find(path);
        if (st != m_seamCorrection.end())
            std::tie(seam, colour) = st->second;
        auto bt = m_blurQuads.find(clipId);
        if (bt != m_blurQuads.end())
            blur = bt->second;
        auto ba = m_blurQuads.find(handOver.afterClipId);
        if (handOver.afterClipId && ba != m_blurQuads.end())
            blurAfter = ba->second;
    }
    if (blurAfter || (blur && handOver.afterClipId))
        blur = [own = std::move(blur), after = std::move(blurAfter),
                untilMs = handOver.untilMs](int64_t ptsMs, std::vector<insv::BlurQuad> &out) {
            if (const stitch::BlurQuadsFn &fn = ptsMs < untilMs ? own : after)
                fn(ptsMs, out);
        };
    src.SetCaptureTiming(std::move(s), handOver.untilMs, std::move(after));
    src.SetSeamCorrection(seam);
    src.SetLensColour(colour);
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

void PlaybackController::switchToSegment(int index)
{
    const Segment &seg = m_decPlaced[index].seg;
    const QString path = seg.sourcePath;
    if (m_activeSource && m_activePath == path) {
        touchSourceLru(path);
        // Gave its memory back for a job meanwhile (makeGpuRoom).
        resumeSource(path, *m_activeSource);
        return;
    }

    // A file known to be gone isn't opened; the view decides how to tell
    // the user (it owns the missing-media marks).
    QString err;
    std::shared_ptr<stitch::IStitchSource> src;
    if (seg.missing)
        err = QStringLiteral("file not found: %1").arg(path);
    else if (!(src = acquireSource(seg, &err)) && err.isEmpty())
        err = QStringLiteral("could not open %1").arg(path);
    if (!src) {
        emit openFailed(path, seg.clipId, err);
        // Black for the clip's span, so playback goes on past it. Cached
        // like the file's own source: the next pass plays it without asking
        // again, and a file that comes back is opened afresh (reopenSources).
        src = std::make_shared<BlankClipSource>(seg.fps, seg.sourceDurationMs, false);
        src->Open(path.toStdString(), openConfig(m_decPreviewConfig, seg));
        m_srcCache[path] = src;
        touchSourceLru(path);
    }
    // The source we leave keeps its lens ring only for frames still queued or
    // on screen; drop the rest of its GPU memory.
    if (m_activeSource)
        m_activeSource->TrimLensFrames();
    m_activeSource = std::move(src);
    m_activePath = path;
    balanceGpuMemory(0, false);
}

void PlaybackController::noteGpuFallback(int clipId)
{
    if (m_fallbackSource.lock() != m_activeSource) {
        m_fallbackSource = m_activeSource;
        m_fallbackSeen = {};
    }
    const stitch::GpuFallback fb = m_activeSource->GetGpuFallback();
    if (fb.decode == m_fallbackSeen.decode && fb.stitch == m_fallbackSeen.stitch &&
        fb.codec == m_fallbackSeen.codec)
        return;
    m_fallbackSeen = fb;
    if (fb.any())
        emit gpuFallback(m_activePath, clipId, fb.decode, fb.stitch,
                         QString::fromStdString(fb.codec));
}

bool PlaybackController::holdFailedRead(const std::string &err, bool playing)
{
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        first = !m_readFailed;
    }
    if (first)
        std::fprintf(stderr, "[playback] a frame of %s didn't read (%s); trying it again\n",
                     qPrintable(m_activePath), err.c_str());
    if (playing && ++m_readRetries > kReadRetries) {
        std::fprintf(stderr, "[playback] %s still doesn't read (%s); going on with the next clip\n",
                     qPrintable(m_activePath), err.c_str());
        m_readRetries = 0;
        std::lock_guard<std::mutex> lk(m_mx);
        m_readFailed = false;
        return false;
    }
    // The card is fuller than its budget says (the read failed with room to
    // spare by it): the coldest idle clip lets go, one per try.
    for (const QString &path : std::as_const(m_srcLru)) {
        if (path == m_activePath)
            continue;
        const auto it = m_srcCache.find(path);
        if (it != m_srcCache.end() && releaseSourceGpu(path, *it->second))
            break;
    }
    setHardStall(true); // cleared by the next read
    std::lock_guard<std::mutex> lk(m_mx);
    m_readFailed = true;
    // Paused, a frame after the one shown isn't wanted until play.
    m_readParked = !playing && m_decLastSourceMs >= 0;
    m_readRetryAt = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(playing ? kReadRetryPlayingMs : kReadRetryPausedMs);
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
    // A file known to be gone has nothing to open ahead of time.
    if ((np.seg.sourcePath == cur.seg.sourcePath && np.seg.inMs == cur.seg.outMs) ||
        np.seg.missing || m_prefetchDeclined == next || !roomForPrefetch(next))
        return;
    launchPrefetch(next, nullptr);
}

bool PlaybackController::roomForPrefetch(int next)
{
    const Placed &np = m_decPlaced[next];
    // A second clip on the graphics card while this one plays: only with
    // room for it. Else the cut opens it once this one has let go
    // (acquireSource), behind "Buffering…".
    const auto cached = m_srcCache.find(np.seg.sourcePath);
    const bool onGpu = np.seg.sourcePath != m_activePath && cached != m_srcCache.end() &&
                       !cached->second->GpuReleased();
    if (!onGpu && !np.seg.flat) {
        const uint64_t need = sourceGpuNeed(np.seg.sourcePath);
        balanceGpuMemory(need, false, np.seg.sourcePath);
        const core::GpuMemory mem = core::sharedDeviceMemory();
        if (mem.known && mem.headroom() < gpuMargin(mem) + need) {
            core::log::Info("[playback] no room on the graphics card to open %s ahead of the "
                            "cut (%.0f MiB free, %.0f MiB wanted)\n",
                            qPrintable(np.seg.sourcePath), toMiB(mem.headroom()),
                            toMiB(gpuMargin(mem) + need));
            m_prefetchDeclined = next;
            return false;
        }
    }
    return true;
}

void PlaybackController::launchPrefetch(int next, std::shared_ptr<LayerLease> lease)
{
    const Placed &np = m_decPlaced[next];
    auto p = std::make_unique<Prefetch>();
    p->segIndex = next;
    p->path = np.seg.sourcePath;
    p->lease = std::move(lease);
    if (m_lentOut && m_lentOut->path == p->path)
        takeBackLentOut(true);
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
                         [this, job, viewportLens, clipId, handOver, seg = np.seg,
                          inMs = np.seg.inMs, config = openConfig(m_decPreviewConfig, np.seg)] {
                             if (!job->src) {
                                 auto src = makeSource(seg);
                                 if (!src->Open(job->path.toStdString(), config)) {
                                     if (job->lease)
                                         job->lease->decline();
                                     return; // the boundary opens it again and reports why
                                 }
                                 applySourceSettings(*src, job->path, clipId, handOver);
                                 job->src = std::move(src);
                             }
                             if (job->lease) {
                                 // The layer renders its view straight from
                                 // the lenses (NativeStitchSource's fused pass).
                                 const auto *native =
                                     dynamic_cast<const stitch::NativeStitchSource *>(job->src.get());
                                 job->lent = native && native->GpuViewAvailable() &&
                                             job->src->SeekTo(job->lease->fromMs()) &&
                                             job->lease->lend(job->src);
                                 if (job->lent) {
                                     job->opened = true;
                                     return;
                                 }
                                 job->lease->decline(); // as before: the clip's first frame
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
    bool atEnd = true;
    if (p->lent) {
        // Back from the layer, its frames still to come read first: the
        // source's next frame is then the clip's first, read now as the job
        // would have. Else (the layer stopped early) a seek to it.
        p->lease->takeBack(true, atEnd);
        if (auto *native = dynamic_cast<stitch::NativeStitchSource *>(p->src.get()))
            native->SetViewPanes({}, {}, 0); // the layer's split screen, if any
        if (atEnd) {
            p->got = readSourceFrame(*p->src,
                                     m_viewport && m_viewport->LensFramesSupported() &&
                                         p->src->LensFramesAvailable(),
                                     p->frame, p->lens);
        } else {
            core::log::Info("[playback] the crossfade's layer gave %s back before its trim-in: "
                            "sought there\n",
                            qPrintable(p->path));
            p->src->SeekTo(m_decPlaced[next].seg.inMs);
        }
    }
    if (m_activeSource)
        m_activeSource->TrimLensFrames(); // as switchToSegment
    m_srcCache[p->path] = p->src; // a jump within the file replaces the active one
    m_activeSource = std::move(p->src);
    m_activePath = p->path;
    touchSourceLru(m_activePath);
    m_decRsGen = ~uint64_t(0); // re-apply: settings may have changed since
    if (atEnd)
        m_heldRead = HeldRead{std::move(p->frame), std::move(p->lens), p->got};
    balanceGpuMemory(0, false);
    return true;
}

void PlaybackController::invalidatePrefetch()
{
    if (m_prefetch) {
        m_prefetch->stale = true;
        if (m_prefetch->lease) {
            // The layer asks again for the frames it wants now.
            bool atEnd = false;
            m_prefetch->lease->takeBack(false, atEnd);
        }
    }
    m_heldRead.reset();
    m_prefetchDeclined = -1;
}

void PlaybackController::serveLayerAsks(bool playing)
{
    std::vector<std::shared_ptr<LayerLease>> asks = m_lender->takeAsks();
    asks.insert(asks.end(), m_layerAsksLater.begin(), m_layerAsksLater.end());
    m_layerAsksLater.clear();
    for (const std::shared_ptr<LayerLease> &lease : asks) {
        const LayerAnswer a = playing ? lendForLayer(lease) : LayerAnswer::Declined;
        if (a == LayerAnswer::Later)
            m_layerAsksLater.push_back(lease);
        else if (a == LayerAnswer::Declined)
            lease->decline(); // paused: a scrub's frame; the layer reads its own
    }
}

PlaybackController::LayerAnswer PlaybackController::lendForLayer(const std::shared_ptr<LayerLease> &lease)
{
    const int next = m_decSegIndex + 1;
    if (!lease->asked() || m_decSegIndex < 0 || next >= m_decPlaced.size())
        return LayerAnswer::Declined;
    if (!lease->leadIn()) {
        // The clip playing, past its trim-out: lent once its last frame is
        // read (lendAtTrimOut). Not when the next clip reads the same file
        // on: that's the source it reads from. Nor when the next clip found
        // no room to open ahead of the cut: it takes this one's at the cut,
        // and the layer had better open its own now than then.
        const Segment &cs = m_decPlaced[m_decSegIndex].seg;
        return wantsPastTrimOut(*lease, cs) && !cs.flat &&
                       m_decPlaced[next].seg.sourcePath != cs.sourcePath && !m_lentOut &&
                       m_prefetchDeclined != next
                   ? LayerAnswer::Later
                   : LayerAnswer::Declined;
    }
    // The clip after the cut, the layer's frames lead up to its trim-in.
    const Segment &seg = m_decPlaced[next].seg;
    if (seg.clipId != lease->clipId() || std::llabs(seg.inMs - lease->endMs()) > 1 ||
        lease->fromMs() >= seg.inMs || seg.flat || seg.missing ||
        seg.sourcePath == m_activePath || m_prefetchDeclined == next)
        return LayerAnswer::Declined;
    if (m_prefetch) {
        if (m_prefetch->done.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return LayerAnswer::Later; // one job at a time
        if (!m_prefetch->stale) {
            // Read ahead already, from the trim-in (the layer asked late: a
            // play started near the cut, or its thread was busy): the
            // layer's frames are behind it. Sought back, if the cut is far
            // enough off for the seek; its first frame read is dropped.
            const Placed &cur = m_decPlaced[m_decSegIndex];
            const qint64 decodedMs =
                m_decLastSourceMs >= 0 ? cur.startMs + cur.seg.localAtSourceMs(m_decLastSourceMs)
                                       : m_decPlaced[next].startMs;
            if (m_prefetch->segIndex != next || m_prefetch->lease || !m_prefetch->src ||
                m_decPlaced[next].startMs - decodedMs < kLayerReseekMs)
                return LayerAnswer::Declined;
            m_srcCache[m_prefetch->path] = std::move(m_prefetch->src); // launchPrefetch takes it
            touchSourceLru(m_prefetch->path);
        }
        m_prefetch.reset();
    }
    if (!roomForPrefetch(next))
        return LayerAnswer::Declined;
    core::log::Info("[playback] %s opens ahead of the cut at the crossfade's start (%lld ms), "
                    "for its layer\n",
                    qPrintable(seg.sourcePath), static_cast<long long>(lease->fromMs()));
    launchPrefetch(next, lease);
    return LayerAnswer::Lent;
}

bool PlaybackController::wantsPastTrimOut(const LayerLease &lease, const Segment &seg) const
{
    // Asked from half a frame before the layer's start, the trim-out.
    const double frameMs = seg.fps > 0.0 ? 1000.0 / seg.fps : 1000.0 / 30.0;
    return lease.clipId() == seg.clipId && lease.fromMs() <= seg.outMs &&
           seg.outMs - lease.fromMs() <= frameMs + 1.0 && lease.endMs() > seg.outMs;
}

bool PlaybackController::lendAtTrimOut(const Placed &cur)
{
    if (m_layerAsksLater.empty() || m_lentOut || m_decLastSourceMs < 0 || !m_activeSource)
        return false;
    // Is the next frame past the trim-out? By the frame rate, as the layer
    // has it (LayerLease): in doubt it's the layer's.
    const double fps = m_activeSource->GetInfo().fps;
    const double frameMs = fps > 0.0 ? 1000.0 / fps : 1000.0 / 30.0;
    if (m_decLastSourceMs + frameMs < cur.seg.outMs - 1.0)
        return false;
    const int next = m_decSegIndex + 1;
    if (next >= m_decPlaced.size() || m_decPlaced[next].seg.sourcePath == m_activePath)
        return false;
    // The next clip comes first: only when it has its place on the card
    // already (opened ahead of the cut, or cached there). Else the cut needs
    // the room this source takes, and the layer reads its own.
    const Segment &ns = m_decPlaced[next].seg;
    const auto cached = m_srcCache.find(ns.sourcePath);
    const bool nextReady =
        ns.flat || (m_prefetch && !m_prefetch->stale && m_prefetch->segIndex == next) ||
        (cached != m_srcCache.end() && !cached->second->GpuReleased());
    auto it = std::find_if(m_layerAsksLater.begin(), m_layerAsksLater.end(),
                           [&](const std::shared_ptr<LayerLease> &l) {
                               return !l->leadIn() && wantsPastTrimOut(*l, cur.seg);
                           });
    if (it == m_layerAsksLater.end())
        return false;
    const std::shared_ptr<LayerLease> lease = *it;
    m_layerAsksLater.erase(it);
    const auto *native = dynamic_cast<const stitch::NativeStitchSource *>(m_activeSource.get());
    if (!nextReady || !native || !native->GpuViewAvailable()) {
        lease->decline();
        return false;
    }
    m_activeSource->TrimLensFrames(); // as switchToSegment: only what's queued or shown
    if (!lease->lend(m_activeSource))
        return false; // the layer left meanwhile: read on as ever
    core::log::Info("[playback] %s goes on past its trim-out for the crossfade's layer\n",
                    qPrintable(m_activePath));
    m_srcCache.erase(m_activePath);
    m_srcLru.removeAll(m_activePath);
    m_lentOut = LentOut{m_activePath, std::move(m_activeSource), lease};
    m_activeSource.reset();
    m_activePath.clear();
    return true;
}

void PlaybackController::takeBackLentOut(bool force)
{
    if (!m_lentOut || (!force && !m_lentOut->lease->done()))
        return;
    bool atEnd = false;
    m_lentOut->lease->takeBack(false, atEnd);
    LentOut l = std::move(*m_lentOut);
    m_lentOut.reset();
    if (auto *native = dynamic_cast<stitch::NativeStitchSource *>(l.src.get()))
        native->SetViewPanes({}, {}, 0); // the layer's split screen, if any
    // Cached again, unless the file was opened afresh meanwhile or has left
    // the timeline.
    const bool onTimeline = std::any_of(m_decPlaced.begin(), m_decPlaced.end(), [&](const Placed &p) {
        return p.seg.sourcePath == l.path;
    });
    if (onTimeline && !m_srcCache.count(l.path) && l.path != m_activePath) {
        m_srcCache.emplace(l.path, std::move(l.src));
        touchSourceLru(l.path);
    }
}

void PlaybackController::goToSegment(int next)
{
    if (takePrefetch(next)) {
        // Its first frame is read already: the next read returns it.
        m_decLastSourceMs = -1;
        m_decSegIndex = next;
        return;
    }
    // Until the next clip's first frame is read: its seek and decode up to
    // the trim-in.
    switchToSegment(next);
    setHardStall(true);
    m_activeSource->SeekTo(m_decPlaced[next].seg.inMs);
    m_decLastSourceMs = -1;
    m_decSegIndex = next;
}

void PlaybackController::decoderLoop()
{
    for (;;) {
        bool doSeek = false;
        bool tlChanged = false;
        bool cfgChanged = false;
        bool makeRoom = false;
        bool makeRoomEvenShown = false;
        QStringList reopen; // m_reopenPaths, taken with the timeline
        qint64 seekMs = 0;
        bool seekToKeyframe = false;
        uint64_t gen = 0;
        bool playing = false;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            const auto ready = [this] {
                const size_t cap = !m_playing   ? kScrubLookahead
                                   : m_lensQueue ? kLensQueueMax
                                                 : queueCapFor(m_frameBytes);
                // A source that gave its memory back for a job while paused
                // comes back for a play or a seek, not for the lookahead.
                const bool parked = !m_playing && m_activeSource && m_activeSource->GpuReleased();
                const bool retryLater =
                    m_readFailed &&
                    (m_readParked || std::chrono::steady_clock::now() < m_readRetryAt);
                const bool canDecode = !m_decPlaced.isEmpty() && !m_decoderEof &&
                                       m_queue.size() < cap && !parked && !retryLater;
                return m_stop || m_seekPending || m_timelineDirty || m_previewConfigDirty ||
                       m_gpuRoomPending || canDecode || m_lender->anyAsks();
            };
            while (!ready()) {
                if (m_readFailed && !m_readParked)
                    m_cv.wait_until(lk, m_readRetryAt);
                else
                    m_cv.wait(lk);
            }
            if (m_stop)
                return;
            if (m_timelineDirty) {
                // A file whose stitch settings changed is opened afresh, as
                // one asked for by reopenSources.
                reopen = std::exchange(m_reopenPaths, {});
                for (const Placed &now : m_placed)
                    for (const Placed &was : m_decPlaced)
                        if (was.seg.sourcePath == now.seg.sourcePath &&
                            (was.seg.lensGuard != now.seg.lensGuard ||
                             was.seg.stitchMethod != now.seg.stitchMethod ||
                             was.seg.seamColourBlend != now.seg.seamColourBlend ||
                             was.seg.seamFlow != now.seg.seamFlow ||
                             was.seg.lensCalibration != now.seg.lensCalibration) &&
                            !reopen.contains(now.seg.sourcePath)) {
                            reopen << now.seg.sourcePath;
                            break;
                        }
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
            makeRoom = std::exchange(m_gpuRoomPending, false);
            makeRoomEvenShown = std::exchange(m_gpuRoomEvenShown, false);
            gen = m_seekGen;
            playing = m_playing;
            if (tlChanged || cfgChanged || doSeek)
                m_readFailed = false; // another frame, or another source
        }
        if (tlChanged || cfgChanged || doSeek) {
            invalidatePrefetch();
            m_readRetries = 0;
        }

        if (tlChanged || cfgChanged)
            takeBackLentOut(true); // the layers ask again for what they want now
        if (tlChanged) {
            pruneSourcesToTimeline(); // drop sources no longer on the timeline
            for (const QString &p : reopen) {
                m_srcCache.erase(p);
                m_srcLru.removeAll(p);
                if (m_activePath == p) {
                    m_activeSource.reset();
                    m_activePath.clear();
                    m_decSegIndex = -1;
                }
            }
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

        if (makeRoom) {
            // Room for a job's clip: the largest any of ours took.
            balanceGpuMemory(sourceGpuNeed({}), makeRoomEvenShown && !playing);
            if (!tlChanged && !cfgChanged && !doSeek)
                continue; // not a read: that would take the memory up again
        } else if (std::chrono::steady_clock::now() - m_lastGpuCheck >= std::chrono::seconds(1)) {
            // Other programs take graphics memory too.
            balanceGpuMemory(0, false);
            retryGpuFallback();
        }

        // After a seek, on the next pass: with the segment it landed in.
        if (!doSeek)
            serveLayerAsks(playing);
        takeBackLentOut(false);

        if (!playing && !doSeek && m_activeSource && m_activeSource->GpuReleased())
            continue; // parked (see the wait above): no lookahead read

        if (m_decPlaced.isEmpty())
            continue; // nothing to play; wait for the next timeline/seek/stop

        if (doSeek) {
            const int idx = segmentAt(m_decPlaced, seekMs);
            const Placed &ps = m_decPlaced[idx];
            const qint64 clamped = std::clamp<qint64>(seekMs, ps.startMs, ps.endMs());
            switchToSegment(idx);
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
            // A scrub's passing frames quickly; its final seek is exact.
            m_activeSource->SetQuickSeek(seekToKeyframe);
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
        // Back on the GPU after making room for a job, before asking what it
        // can do: a released source has no lens frames.
        resumeSource(m_activePath, *m_activeSource);
        // A crossfade's second half wants this clip's frames past its
        // trim-out: lent there instead of read on.
        if (playing && !m_heldRead && lendAtTrimOut(cur)) {
            goToSegment(m_decSegIndex + 1);
            continue;
        }

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
        noteGpuFallback(cur.seg.clipId);
        const auto t1 = std::chrono::steady_clock::now();
        if (render::framestats::Enabled() && got) {
            std::lock_guard<std::mutex> lk(m_mx);
            m_statsReadMs.add(render::framestats::MsBetween(t0, t1));
        }
        // SeekTo() only positions the decoder; this first read after it does
        // the decoding up to the target, the slow part of a seek or boundary.
        setHardStall(false);

        // A read that failed is not the clip's end: going on to the next
        // clip would leave the decoder clips ahead of the playhead, each
        // failing the same way while the graphics memory is full.
        if (!got && !m_activeSource->GetLastError().empty() &&
            holdFailedRead(m_activeSource->GetLastError(), playing))
            continue;
        if (got) {
            bool failed = false;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                failed = std::exchange(m_readFailed, false);
            }
            if (failed)
                core::log::Info("[playback] %s reads again\n", qPrintable(m_activePath));
            m_readRetries = 0;
        }

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
            } else {
                goToSegment(next);
                continue;
            }
        }

        // Rewrite the frame's timestamp from source time to timeline time.
        const qint64 sourceMs = frame.timestamp_ms;
        const qint64 sourceUs = frame.timestamp_us;
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
                               sourceUs, cur.seg.clipId});
        }
    }
}
