#include "LayerLease.h"

#include <utility>

LayerLease::LayerLease(int clipId, int64_t fromMs, int64_t endMs, bool leadIn, Render render,
                       std::function<void()> answered)
    : m_clipId(clipId), m_fromMs(fromMs), m_endMs(endMs), m_leadIn(leadIn),
      m_answered(std::move(answered)), m_render(std::move(render))
{
}

bool LayerLease::lend(std::shared_ptr<stitch::IStitchSource> src)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_state != State::Asked)
            return false;
        const double fps = src->GetInfo().fps;
        if (fps > 0.0)
            m_frameMs = 1000.0 / fps;
        m_src = std::move(src);
        m_state = State::Lent;
    }
    if (m_answered)
        m_answered();
    return true;
}

void LayerLease::decline()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_state != State::Asked)
            return;
        m_state = State::Declined;
    }
    if (m_answered)
        m_answered();
}

bool LayerLease::asked() const
{
    std::lock_guard<std::mutex> lk(m_mx);
    return m_state == State::Asked;
}

bool LayerLease::done() const
{
    std::lock_guard<std::mutex> lk(m_mx);
    return m_state == State::Left || (m_state == State::Lent && !nextIsLayers());
}

bool LayerLease::nextIsLayers() const
{
    const double next = m_lastMs < 0 ? double(m_fromMs) : m_lastMs + m_frameMs;
    return !m_overran && !m_failed && next < m_endMs - 1.0;
}

bool LayerLease::readOne(exportpipe::VideoFrame &out)
{
    if (!m_render || !m_src || !nextIsLayers())
        return false;
    if (!m_render(*m_src, out)) {
        m_failed = true;
        return false;
    }
    m_lastMs = out.ptsMs;
    if (out.ptsMs >= m_endMs) {
        m_overran = true; // playback's frame: dropped, and playback seeks
        return false;
    }
    return true;
}

std::shared_ptr<stitch::IStitchSource> LayerLease::takeBack(bool finish, bool &atEnd)
{
    std::lock_guard<std::mutex> lk(m_mx);
    const bool lent = m_state == State::Lent || m_state == State::Left;
    if (finish && m_state == State::Lent) {
        while (m_buffer.size() < kMaxBuffered) {
            exportpipe::VideoFrame f;
            if (!readOne(f))
                break;
            m_buffer.push_back(std::move(f));
        }
    }
    m_reachedEnd = lent && !m_overran && !m_failed && !nextIsLayers();
    atEnd = m_reachedEnd;
    m_state = State::Back;
    return std::move(m_src);
}

LayerLease::Next LayerLease::next(exportpipe::VideoFrame &out)
{
    std::lock_guard<std::mutex> lk(m_mx);
    if (!m_buffer.empty()) {
        out = std::move(m_buffer.front());
        m_buffer.pop_front();
        return Next::Frame;
    }
    switch (m_state) {
    case State::Asked:
        return Next::Waiting;
    case State::Declined:
    case State::Left:
        return Next::Gone;
    case State::Back:
        return m_reachedEnd ? Next::End : Next::Gone;
    case State::Lent:
        break;
    }
    if (readOne(out))
        return Next::Frame;
    return Next::End; // the layer's last frame was read (or a read failed)
}

void LayerLease::leave()
{
    std::lock_guard<std::mutex> lk(m_mx);
    m_render = nullptr;
    m_buffer.clear();
    if (m_state == State::Lent)
        m_state = State::Left;
    else if (m_state == State::Asked)
        m_state = State::Declined; // not wanted: playback's decoder skips it
}

LayerLender::LayerLender(std::function<void()> wakePlayback, std::function<void()> answered)
    : m_wake(std::move(wakePlayback)), m_answered(std::move(answered))
{
}

std::shared_ptr<LayerLease> LayerLender::ask(int clipId, int64_t fromMs, int64_t endMs,
                                             bool leadIn, LayerLease::Render render)
{
    auto lease = std::make_shared<LayerLease>(clipId, fromMs, endMs, leadIn, std::move(render),
                                              m_answered);
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_asks.push_back(lease);
    }
    if (m_wake)
        m_wake();
    return lease;
}

std::vector<std::shared_ptr<LayerLease>> LayerLender::takeAsks()
{
    std::lock_guard<std::mutex> lk(m_mx);
    return std::exchange(m_asks, {});
}

bool LayerLender::anyAsks() const
{
    std::lock_guard<std::mutex> lk(m_mx);
    return !m_asks.empty();
}
