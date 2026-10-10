// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "NoiseReduction.h"

#include <memory>
#include <string>

namespace insv {

// A streaming noise filter over interleaved float PCM, as ClipAudioReader
// runs it: output is the input delayed by exactly latency() frames (the
// first latency() output frames after reset() are silence). Not thread-safe.
class NoiseFilter {
public:
    virtual ~NoiseFilter() = default;

    // 0 = no change at all, 1 = the full effect. Takes effect from the next
    // STFT frame.
    virtual void setStrength(float strength) = 0;
    // Forget all state (after a seek).
    virtual void reset() = 0;
    // Start the input over from its beginning but keep what was learned
    // about the noise, held fixed over the input already seen and adapting
    // again after it. ClipAudioReader does this at a file's start, where
    // there's nothing before the target to learn from: a first pass over
    // the opening seconds, then the real one from frame 0. By default a
    // plain reset().
    virtual void rewind() { reset(); }
    virtual int latency() const = 0;
    // How much input it needs before its output settles (its trackers and
    // noise estimates): ClipAudioReader runs it in over this much before a
    // seek target, so playback after a seek matches export.
    virtual double warmupSeconds() const { return 0.5; }
    // `frames` interleaved frames in, the same number out (`in` == `out` is
    // allowed).
    virtual void process(const float* in, float* out, int frames) = 0;
};

// The filter for `mode`; null for NoiseMode::Off (and for the voice modes
// in a build without ONNX Runtime).
std::unique_ptr<NoiseFilter> makeNoiseFilter(NoiseMode mode, int sampleRate, int channels);

// Whether the voice modes can run: built with ONNX Runtime and the
// DeepFilterNet model files found. Cheap (doesn't load the model).
bool voiceModesAvailable(std::string* why = nullptr);
// voiceModesAvailable(), and ONNX Runtime and the model really load (loads
// them now if needed; VoiceIsolator::ready). For diagnostics.
bool voiceModesReady(std::string* why = nullptr);

} // namespace insv
