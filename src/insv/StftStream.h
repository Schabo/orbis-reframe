// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

// Internal to the STFT noise filters (WindReducer, GeneralWindReducer,
// HissHumReducer, VoiceIsolator): the streaming around their per-frame
// work. PcmInput and PcmOutput are the input and output queues every filter
// has; StftStream is the sqrt-Hann overlap-add loop of the three that turn
// each frame's spectrum down by one gain per bin (VoiceIsolator frames and
// synthesises its own way, for the model).

#include "Rdft.h"

#include <cstdint>
#include <vector>

namespace insv {

// Input per channel, held from some absolute frame on; frames are counted
// from the first push after construction or clear().
class PcmInput {
public:
    explicit PcmInput(int channels);

    // `frames` interleaved frames.
    void push(const float* pcm, int frames);
    // One interleaved frame.
    void pushFrame(const float* x);
    // Channel c at absolute frame i: 0 before the first frame and past the
    // last pushed.
    float at(int c, int64_t i) const
    {
        const int64_t k = i - m_base;
        const auto& v = m_in[static_cast<size_t>(c)];
        return (k >= 0 && k < static_cast<int64_t>(v.size())) ? v[static_cast<size_t>(k)] : 0.0f;
    }
    // Frames pushed in total.
    int64_t count() const { return m_count; }
    // Forget the input before `frame` (nothing will read it again), once
    // that is more than `minDrop` frames, so the erase stays rare.
    void dropBefore(int64_t frame, int64_t minDrop);
    void clear();

private:
    std::vector<std::vector<float>> m_in;
    int64_t m_base = 0;
    int64_t m_count = 0;
};

// Finished output, interleaved, handed out behind a fixed leading delay:
// the first `latency` frames popped are silence, as NoiseFilter promises.
class PcmOutput {
public:
    PcmOutput(int channels, int latency);

    // One sample, in interleaved order.
    void push(float v) { m_ready.push_back(v); }
    void pop(float* out, int frames);
    void clear();

private:
    const int m_channels;
    const int m_latency;
    std::vector<float> m_ready;
    size_t m_readyPos = 0; // floats already handed out
    int64_t m_outCount = 0; // frames handed out, the leading silence included
};

// A streaming STFT with sqrt-Hann analysis and synthesis windows at 75 %
// overlap (hop = fft / 4). The first frame starts fft - hop before input
// frame 0, so sample 0 already gets the full overlap-add; output [start,
// start + hop) is final once the frame at `start` is done. A frame runs once
// the input reaches start + latency: latency is fft or more, the more being
// the filter's own look-ahead past the frame.
class StftStream {
public:
    StftStream(int fft, int latency, int channels);
    StftStream(const StftStream&) = delete;
    StftStream& operator=(const StftStream&) = delete;

    bool ok() const { return m_tx.ok(); }
    int fft() const { return m_fft; }
    int hop() const { return m_hop; }
    int bins() const { return m_fft / 2 + 1; }

    PcmInput& input() { return m_input; }
    const PcmInput& input() const { return m_input; }
    // The frame being run: each channel's spectrum, bins() values.
    const std::vector<std::vector<AVComplexFloat>>& spectra() const { return m_spec; }
    // The start of the next frame to run.
    int64_t nextFrame() const { return m_next; }

    // Runs every frame the input allows: each channel's spectrum into
    // spectra(), then `gains(start)` returns bins() gains, applied to every
    // channel, and the frame is overlap-added. Then forgets the input before
    // nextFrame() - keepBack (keepBack: how far before its start a frame
    // reads, beyond its own window).
    template <class Gains>
    void run(Gains&& gains, int64_t keepBack = 0)
    {
        while (m_input.count() >= m_next + m_latency) {
            analyse(m_next);
            synthesise(m_next, gains(m_next));
            m_next += m_hop;
        }
        m_input.dropBefore(m_next - keepBack, 4 * static_cast<int64_t>(m_fft));
    }

    void pop(float* out, int frames) { m_output.pop(out, frames); }
    // Back to before input frame 0: input, overlap-add and output cleared.
    void reset();

private:
    void analyse(int64_t start);
    void synthesise(int64_t start, const float* gain);

    const int m_fft;
    const int m_hop;
    const int m_latency;
    const int m_channels;
    Rdft m_tx;
    const std::vector<float> m_win;
    PcmInput m_input;
    PcmOutput m_output;
    int64_t m_next;
    std::vector<std::vector<AVComplexFloat>> m_spec;
    std::vector<std::vector<float>> m_acc; // overlap-add, per channel
};

} // namespace insv
