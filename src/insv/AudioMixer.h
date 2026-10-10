// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "NoiseReduction.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace insv {

class ClipAudioReader;

// One clip on an extra audio track, in output-rate frames.
struct MixClip {
    std::string path;
    int stream = -1;          // InsvAudioDemux::open's ordinal, -1 = default
    int64_t startFrame = 0;   // timeline frame of the clip's first sample
    int64_t lengthFrames = 0; // its span on the timeline
    int64_t inFrame = 0;      // source frame (at the output rate) at startFrame
    double speed = 1.0;       // pitch-preserving stretch (ClipAudioReader::setTempo)
    float gain = 1.0f;        // linear: the clip's level times its track's
    int64_t fadeInFrames = 0;
    int64_t fadeOutFrames = 0;
    NoiseReduction noise; // the clip's own (ClipAudioReader::setNoiseReduction)
};

// Sums the extra audio tracks' clips into the timeline's PCM. Shared by
// playback (AudioPlayer's decoder thread) and export (ReframeExportController)
// so the two mix identically: the same clip placement, fades and limiter.
// FFmpeg-only, no Qt. Not thread-safe -- one owner thread.
class AudioMixer {
public:
    AudioMixer(int sampleRate, int channels);
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Replace the clips (drops every open reader).
    void setClips(std::vector<MixClip> clips);
    bool empty() const { return m_voices.empty(); }

    // Add every clip's audio over timeline frames [start, start + frames) to
    // `out` (interleaved, `frames * channels` floats). Calls needn't be
    // contiguous: a clip whose reader isn't where the call starts is re-seeked.
    void mixInto(float* out, int64_t start, int64_t frames);

    // A stateless soft limiter: samples within -1 dBFS pass unchanged, louder
    // ones are bent smoothly (tanh) towards, never past, full scale. Applied
    // to the final mix -- the clips' own audio included -- in both playback
    // and export.
    static void limit(float* pcm, int64_t samples);

private:
    struct Voice {
        MixClip clip;
        std::unique_ptr<ClipAudioReader> reader;
        int64_t nextFrame = -1; // timeline frame the reader will return next
    };
    const int m_rate;
    const int m_channels;
    std::vector<Voice> m_voices;
    std::vector<float> m_tmp;
};

} // namespace insv
