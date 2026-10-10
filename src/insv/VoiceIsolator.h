// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "NoiseFilter.h"

#include <memory>
#include <string>

namespace insv {

// "Voice only" / "Voice + ambience": DeepFilterNet 3 speech enhancement
// (Schröter et al., MIT/Apache-2.0; its ONNX export run with ONNX Runtime).
// It keeps speech and takes out everything else -- engine, wind, road,
// music. "Voice + ambience" mixes some of the original back in, so the rest
// is turned down evenly rather than removed.
//
// The DSP around the model is libDF's, checked against the reference
// `deep-filter` CLI (same output to -67 dB): 960-point STFT at 48 kHz (hop
// 480, Vorbis window), 32 ERB-band features (mean-normalised dB) and the
// lowest 96 bins as complex features (unit-normalised); the model gives ERB
// gains for the whole spectrum and a 5-tap complex "deep filter" for the
// lowest 96 bins (4.8 kHz), both two frames ahead of the audio.
//
// The ONNX models take a whole sequence at once (their recurrent state isn't
// an input), so it runs in chunks: 3 s of output per run, with 2 s of the
// audio before it run in first to warm the model up, and 0.5 s crossfaded
// into the next chunk (a model started at a different point settles on
// slightly different gains, so chunks don't meet exactly). Latency is 3.53 s
// -- ClipAudioReader reads ahead, so playback doesn't notice.
//
// 48 kHz only (the app's rate); any other rate, or no model, passes the audio
// through unchanged (still delayed by latency()).
class VoiceIsolator : public NoiseFilter {
public:
    enum class Kind {
        VoiceOnly,     // strength 1 -> noise down 60 dB
        VoiceAmbience, // strength 1 -> 24 dB (0.5, the default: 12 dB)
    };

    VoiceIsolator(int sampleRate, int channels, Kind kind);
    ~VoiceIsolator() override;
    VoiceIsolator(const VoiceIsolator&) = delete;
    VoiceIsolator& operator=(const VoiceIsolator&) = delete;

    // Built with ONNX Runtime and the model files are there. Cheap: doesn't
    // load the model (that happens on first use, once per process).
    static bool available(std::string* why = nullptr);
    // Whether the voice modes really run: available(), and ONNX Runtime and
    // the model load. Loads them if that hasn't happened yet (once per
    // process, ~0.5 s) -- for diagnostics (Help > About), not the UI's
    // cheap checks.
    static bool ready(std::string* why = nullptr);
    // Where enc.onnx, erb_dec.onnx and df_dec.onnx are. Unset: the
    // ORBIS_DFN_MODEL_DIR environment variable, else none (not available).
    // The app sets its bundled models/deepfilternet3. Before first use.
    static void setModelDirectory(const std::string& dir);

    void setStrength(float strength) override;
    void reset() override;
    int latency() const override;
    double warmupSeconds() const override { return 2.0; }
    void process(const float* in, float* out, int frames) override;

    // The attenuation limit the strength gives, dB (tests, UI).
    float attenuationDb() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    const Kind m_kind;
    float m_strength;
};

} // namespace insv
