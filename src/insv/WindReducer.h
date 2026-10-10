// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "NoiseFilter.h"

#include <memory>
#include <vector>

namespace insv {

// "MC: Wind reduction" (reduce wind, keep the engine) noise reduction (tuned
// in tools/audio_filter_bench, 2026-09-29). A
// streaming STFT filter over interleaved float PCM:
//  - it tracks the engine's firing fundamental (a harmonic-sum tracker over
//    18-250 Hz on a finer, 16384-point spectrum) and leaves the bins on its
//    harmonics alone;
//  - everything else below ~3 kHz (full below 800 Hz, fading out by 3 kHz)
//    is turned down by `strength`, and further where a bin jumps well above
//    its own slowly tracked floor (a gust); never by more than 24 dB;
//  - one gain per bin, from the channels' average, applied to every channel.
// X5 streams are dual mono, so that is exact there.
//
// Output is the input delayed by exactly latency() frames (the filter needs
// that much look-ahead); the first latency() output frames after reset() are
// silence. A caller that wants output aligned with a source position feeds
// latency() frames ahead of it (ClipAudioReader does). Not thread-safe.
class WindReducer : public NoiseFilter {
public:
    static constexpr float kDefaultStrength = NoiseReduction::kDefaultStrength;

    WindReducer(int sampleRate, int channels);
    ~WindReducer() override;
    WindReducer(const WindReducer&) = delete;
    WindReducer& operator=(const WindReducer&) = delete;

    // 0 = no change at all (the gust cut scales down with it below the
    // default), 1 = the full cut. Takes effect from the next frame.
    void setStrength(float strength) override;
    float strength() const { return m_strength; }

    // Forget all state (after a seek).
    void reset() override;

    int latency() const override;

    // `frames` interleaved frames in, the same number out (`in` == `out` is
    // allowed).
    void process(const float* in, float* out, int frames) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    float m_strength = kDefaultStrength;
};

} // namespace insv
