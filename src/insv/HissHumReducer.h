// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "NoiseFilter.h"

#include <memory>

namespace insv {

// "Hiss / hum": takes out steady background noise -- hiss, fan or air
// noise, mains hum -- and keeps what comes and goes or stands clear as a
// tone (voices, music). Meant for quiet scenes: a steady engine counts as
// background too (its harmonics sit closer than the 23 Hz bins), so on
// riding audio it turns the whole ride down (~7 dB on sample rides, engine
// included). Two stages, both streaming over interleaved float PCM:
//  - hum: every ~85 ms a 32768-point spectrum of the input (0.68 s) is
//    searched for a mains fundamental near 50 or 60 Hz (+-0.3 Hz) and its
//    harmonics up to ~1 kHz. It counts as hum only if the harmonics stand
//    well above their neighbourhood, the frequency holds within 0.1 Hz
//    for a second (an engine's drifts with the revs, even cruising), and
//    nothing sits between the harmonics (an engine at 25 Hz has 50, 100,
//    ... too, but also 75, 125, ...). Each
//    harmonic that stands out is then notched (a ~2 Hz wide band-reject,
//    up to 40 dB deep x strength), sample by sample, before the STFT;
//  - hiss: an STFT filter (2048 points, ~43 ms at 48 kHz). The noise
//    estimate per bin is the spectrum's floor between tones (a minimum over
//    ~+-70 Hz) at its quietest over the last ~1.5 s, so tones and anything
//    that comes and goes stay above it. A Wiener gain from a
//    decision-directed SNR (smooth, little "musical noise") turns each bin
//    down, never by more than 24 dB x strength.
// Strength 0 changes nothing.
class HissHumReducer : public NoiseFilter {
public:
    static constexpr float kDefaultStrength = NoiseReduction::kDefaultStrength;

    HissHumReducer(int sampleRate, int channels);
    ~HissHumReducer() override;
    HissHumReducer(const HissHumReducer&) = delete;
    HissHumReducer& operator=(const HissHumReducer&) = delete;

    void setStrength(float strength) override;
    float strength() const { return m_strength; }
    void reset() override;
    void rewind() override;
    int latency() const override;
    double warmupSeconds() const override { return 2.0; }
    void process(const float* in, float* out, int frames) override;

    // The hum found so far (tests, diagnostics): its fundamental in Hz (0
    // while none) and how sure, 0..1.
    float humHz() const;
    float humConfidence() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    float m_strength = kDefaultStrength;
};

} // namespace insv
