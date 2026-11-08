#include "LentClipView.h"

#include <Log.h>
#include <NativeStitchSource.h>
#include <VulkanHwDevice.h> // core::sharedDeviceMemory

#include <algorithm>
#include <cmath>
#include <utility>

LentClipView::LentClipView(ClipViewSource::Spec spec, std::shared_ptr<LayerLender> lender,
                           int clipId, int64_t startMs, int64_t endMs, bool leadIn)
    : m_view(std::make_shared<ClipViewSource>(std::move(spec))), m_lender(std::move(lender)),
      m_clipId(clipId), m_startMs(startMs), m_endMs(endMs), m_leadIn(leadIn)
{
}

LentClipView::~LentClipView()
{
    if (m_lease)
        m_lease->leave();
}

bool LentClipView::open(std::string &)
{
    // Asked first, from the layer's start (where it's opened ahead of, as
    // it plays in): the stabilizer takes a few hundred ms to build, and
    // playback gets ready for the cut meanwhile. The source comes with the
    // first frame.
    const double halfFrame = 500.0 / std::max(1.0, fps());
    ask(std::llround(m_startMs - halfFrame));
    m_view->prepare();
    return true;
}

const std::string &LentClipView::lastError() const
{
    return m_own ? m_view->lastError() : m_error;
}

void LentClipView::ask(int64_t fromMs)
{
    if (m_lease)
        m_lease->leave();
    m_haveCur = m_haveNext = false;
    m_lastMs = -1;
    // Playback lends only a NativeStitchSource (one with the fused view pass).
    m_lease = m_lender->ask(m_clipId, std::max<int64_t>(0, fromMs), m_endMs, m_leadIn,
                            [view = m_view](stitch::IStitchSource &src, exportpipe::VideoFrame &out) {
                                return view->readFrom(static_cast<stitch::NativeStitchSource &>(src),
                                                      out);
                            });
}

const exportpipe::VideoFrame *LentClipView::fallBack(int64_t ms)
{
    if (m_lease) {
        m_lease->leave();
        m_lease.reset();
    }
    m_own = true;
    m_haveCur = m_haveNext = false;
    // Playback's clips come first on the graphics card: decoded there only
    // with room for this one and another clip playback may open beside it
    // (the next one, at the cut). Else on the processor.
    const core::GpuMemory mem = core::sharedDeviceMemory();
    const uint64_t clip = core::clipGpuBytes();
    const bool cpuDecode = mem.known && clip > 0 && mem.headroom() < core::gpuMargin(mem) + 2 * clip;
    core::log::Info("[inset] a crossfade's layer opens its own stitch of clip %d at %lld ms%s\n",
                    m_clipId, static_cast<long long>(ms),
                    cpuDecode ? ", decoded on the processor: the graphics card keeps its room for "
                                "playback"
                              : "");
    std::string err;
    if (!m_view->openOwn(err, cpuDecode)) {
        m_error = err;
        m_own = false; // nothing to read: lastError() says why
        return nullptr;
    }
    return m_view->frameAt(ms);
}

const exportpipe::VideoFrame *LentClipView::frameAt(int64_t ms)
{
    if (m_own)
        return m_view->frameAt(ms);
    m_waiting = false;
    const double halfFrame = 500.0 / std::max(1.0, fps());
    // As ClipViewSource: the frame whose window [pts - half, pts + half)
    // holds `ms`. A lent source only reads on: behind what was read, or well
    // ahead of it (a seek), ask again from there.
    const int64_t from = std::llround(ms - halfFrame);
    const bool jump = m_haveCur ? (ms + halfFrame < m_cur.ptsMs || ms > m_cur.ptsMs + 1000)
                                : (m_lease && from + 1 < m_lease->fromMs());
    if (!m_lease || jump)
        ask(from);
    if (!m_haveCur) {
        switch (m_lease->next(m_cur)) {
        case LayerLease::Next::Frame:
            m_haveCur = true;
            break;
        case LayerLease::Next::Waiting:
            m_waiting = true; // InsetFeed asks again once answered
            return nullptr;
        case LayerLease::Next::End:
            return nullptr;
        case LayerLease::Next::Gone:
            return fallBack(ms);
        }
    }
    for (;;) {
        if (!m_haveNext) {
            if (m_lease->next(m_next) != LayerLease::Next::Frame)
                break; // its end (or taken back): the last one stays
            m_haveNext = true;
        }
        if (m_next.ptsMs - halfFrame > ms)
            break;
        std::swap(m_cur, m_next);
        m_haveNext = false;
    }
    m_lastMs = std::max(m_lastMs, m_cur.ptsMs);
    return &m_cur;
}

bool LentClipView::readNext(exportpipe::VideoFrame &out)
{
    if (m_own)
        return m_view->readNext(out);
    if (m_haveNext) {
        out = std::move(m_next);
        m_haveNext = false;
        m_lastMs = out.ptsMs;
        return true;
    }
    if (!m_lease)
        return false;
    switch (m_lease->next(out)) {
    case LayerLease::Next::Frame:
        m_lastMs = out.ptsMs;
        return true;
    case LayerLease::Next::Gone: {
        // Taken back before the layer's end (the layer fell behind): on from
        // its own source, at the frame after the last one read.
        const int64_t after = (m_lastMs >= 0 ? m_lastMs : m_lease->fromMs()) +
                              std::llround(1000.0 / std::max(1.0, fps()));
        const exportpipe::VideoFrame *v = fallBack(after);
        if (!v)
            return false;
        out = *v;
        return true;
    }
    default:
        return false;
    }
}
