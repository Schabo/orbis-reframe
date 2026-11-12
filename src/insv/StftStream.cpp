// SPDX-License-Identifier: MIT OR Apache-2.0
#include "StftStream.h"

#include <algorithm>
#include <cstring>

namespace insv {

PcmInput::PcmInput(int channels)
    : m_in(static_cast<size_t>(std::max(1, channels)))
{
}

void PcmInput::push(const float* pcm, int frames)
{
    const size_t ch = m_in.size();
    for (int i = 0; i < frames; ++i)
        for (size_t c = 0; c < ch; ++c)
            m_in[c].push_back(pcm[static_cast<size_t>(i) * ch + c]);
    m_count += frames;
}

void PcmInput::pushFrame(const float* x)
{
    for (size_t c = 0; c < m_in.size(); ++c)
        m_in[c].push_back(x[c]);
    ++m_count;
}

void PcmInput::dropBefore(int64_t frame, int64_t minDrop)
{
    const int64_t drop = frame - m_base;
    if (drop <= minDrop)
        return;
    for (auto& c : m_in)
        c.erase(c.begin(), c.begin() + drop);
    m_base += drop;
}

void PcmInput::clear()
{
    for (auto& c : m_in)
        c.clear();
    m_base = 0;
    m_count = 0;
}

PcmOutput::PcmOutput(int channels, int latency)
    : m_channels(std::max(1, channels))
    , m_latency(latency)
{
}

void PcmOutput::pop(float* out, int frames)
{
    const size_t ch = static_cast<size_t>(m_channels);
    for (int i = 0; i < frames; ++i, ++m_outCount) {
        float* o = out + static_cast<size_t>(i) * ch;
        if (m_outCount < m_latency || m_readyPos + ch > m_ready.size()) {
            std::fill(o, o + ch, 0.0f); // the leading delay
            continue;
        }
        std::copy_n(m_ready.data() + m_readyPos, ch, o);
        m_readyPos += ch;
    }
    if (m_readyPos > m_ready.size() / 2 && m_readyPos > 1 << 16) {
        m_ready.erase(m_ready.begin(), m_ready.begin() + static_cast<std::ptrdiff_t>(m_readyPos));
        m_readyPos = 0;
    }
}

void PcmOutput::clear()
{
    m_ready.clear();
    m_readyPos = 0;
    m_outCount = 0;
}

StftStream::StftStream(int fft, int latency, int channels)
    : m_fft(fft)
    , m_hop(fft / 4)
    , m_latency(latency)
    , m_channels(std::max(1, channels))
    , m_tx(fft, true)
    , m_win(sqrtHann(fft))
    , m_input(channels)
    , m_output(channels, latency)
    , m_next(-(fft - fft / 4))
    , m_spec(static_cast<size_t>(m_channels), std::vector<AVComplexFloat>(static_cast<size_t>(bins())))
    , m_acc(static_cast<size_t>(m_channels), std::vector<float>(static_cast<size_t>(fft), 0.0f))
{
}

void StftStream::reset()
{
    m_input.clear();
    m_output.clear();
    m_next = -(m_fft - m_hop);
    for (auto& a : m_acc)
        std::fill(a.begin(), a.end(), 0.0f);
}

void StftStream::analyse(int64_t start)
{
    for (int c = 0; c < m_channels; ++c) {
        for (int i = 0; i < m_fft; ++i)
            m_tx.real[i] = m_input.at(c, start + i) * m_win[i];
        m_tx.forward();
        std::copy(m_tx.cplx, m_tx.cplx + bins(), m_spec[c].begin());
    }
}

void StftStream::synthesise(int64_t start, const float* gain)
{
    const int nb = bins();
    for (int c = 0; c < m_channels; ++c) {
        const auto& s = m_spec[c];
        for (int k = 0; k < nb; ++k) {
            m_tx.cplx[k].re = s[k].re * gain[k];
            m_tx.cplx[k].im = s[k].im * gain[k];
        }
        m_tx.inverse();
        float* a = m_acc[c].data(); // sqrt-Hann twice at 75 % overlap sums to 2
        for (int i = 0; i < m_fft; ++i)
            a[i] += m_tx.real[i] * m_win[i] * 0.5f;
    }

    // [start, start + hop) is final: hand it on (from sample 0), shift.
    const int64_t from = std::max<int64_t>(start, 0);
    for (int64_t t = from; t < start + m_hop; ++t)
        for (int c = 0; c < m_channels; ++c)
            m_output.push(m_acc[c][static_cast<size_t>(t - start)]);
    for (auto& a : m_acc) {
        std::memmove(a.data(), a.data() + m_hop, sizeof(float) * (m_fft - m_hop));
        std::fill(a.end() - m_hop, a.end(), 0.0f);
    }
}

} // namespace insv
