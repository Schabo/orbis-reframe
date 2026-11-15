// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <algorithm>

namespace insv {

// Per-clip audio noise reduction: chosen per mic clip -- a video clip's own sound or a clip on an
// audio track -- and applied by ClipAudioReader in playback and export alike.
// Header-only and dependency-free, so the edit model (src/core) stores it as
// is.
enum class NoiseMode {
    Off,
    // "MC: Wind reduction" (WindReducer): reduce wind, keep a motorcycle
    // engine's harmonics. "wind" in the .orp (its name before Wind existed).
    MotorcycleWind,
    // "Wind reduction" (GeneralWindReducer): wind only, whatever else is
    // there. "windGeneral" in the .orp.
    Wind,
    // "Hiss / hum" (HissHumReducer): steady background noise and mains hum,
    // for quiet scenes. "hiss" in the .orp.
    Hiss,
    // "Voice only" (VoiceIsolator, DeepFilterNet): keeps speech, removes the
    // rest. "voice" in the .orp.
    Voice,
    // "Voice + ambience": the same, with the rest turned down (12 dB at the
    // default strength) instead of removed. "voiceAmbience" in the .orp.
    VoiceAmbience,
};

struct NoiseReduction {
    // 0.7: the user's pick from tools/audio_filter_bench's listening test
    // (2026-09-29).
    static constexpr float kDefaultStrength = 0.7f;

    NoiseMode mode = NoiseMode::Off;
    float strength = kDefaultStrength; // 0..1, used when mode != Off

    // What the Audio panel sets the strength to when `m` is picked: 0.5 for
    // Hiss / hum (0.7 sounded too strong), 1 for Voice only (all
    // of the noise, 60 dB), 0.5 for Voice + ambience (12 dB), 0.7 for the
    // wind modes.
    static constexpr float defaultStrength(NoiseMode m)
    {
        switch (m) {
        case NoiseMode::Hiss:
        case NoiseMode::VoiceAmbience:
            return 0.5f;
        case NoiseMode::Voice:
            return 1.0f;
        default:
            return kDefaultStrength;
        }
    }

    // This with the strength clamped to [0, 1] (the edit model stores it so).
    NoiseReduction clamped() const
    {
        return {mode, std::clamp(strength, 0.0f, 1.0f)};
    }

    bool operator==(const NoiseReduction& o) const
    {
        return mode == o.mode && strength == o.strength;
    }
    bool operator!=(const NoiseReduction& o) const { return !(*this == o); }
};

} // namespace insv
