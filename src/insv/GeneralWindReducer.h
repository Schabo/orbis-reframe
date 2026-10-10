// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "NoiseFilter.h"

#include <memory>

namespace insv {

// "Wind reduction": wind noise reduction for any sound, engine or not (the
// motorcycle-specific one, which keeps an engine's harmonics, is
// WindReducer). A streaming STFT filter over interleaved float PCM, short
// enough for speech (2048 points, ~43 ms at 48 kHz):
//  - per frame, the spectrum's floor between tones (a minimum over ~±70 Hz
//    of the lightly time-smoothed power) stands in for the noise; tones --
//    a voice's or an engine's harmonics, music -- stand above it;
//  - wind is a floor that falls steeply with frequency (it is almost all
//    below a few hundred Hz). A line fitted to the floor over 80-1000 Hz in
//    dB per octave says how wind-like the frame is: none at -3 dB/octave or
//    flatter (hiss, voice, road roar), fully at -7 or steeper;
//  - the wind estimate is that floor (capped by the fitted line above
//    1 kHz, faded out by 4 kHz), scaled by how wind-like; each bin is turned
//    down by it (spectral subtraction, 2x over-subtracted at full strength),
//    never by more than 30 dB x strength;
//  - one gain per bin, from the channels' average, applied to every channel.
// No wind -> no change: a frame that isn't wind-like passes through.
class GeneralWindReducer : public NoiseFilter {
public:
    static constexpr float kDefaultStrength = NoiseReduction::kDefaultStrength;

    GeneralWindReducer(int sampleRate, int channels);
    ~GeneralWindReducer() override;
    GeneralWindReducer(const GeneralWindReducer&) = delete;
    GeneralWindReducer& operator=(const GeneralWindReducer&) = delete;

    void setStrength(float strength) override;
    float strength() const { return m_strength; }
    void reset() override;
    int latency() const override;
    void process(const float* in, float* out, int frames) override;

    // How wind-like the last frame was, 0..1 (tests, diagnostics).
    float lastWindConfidence() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    float m_strength = kDefaultStrength;
};

} // namespace insv
