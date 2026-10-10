// SPDX-License-Identifier: MIT OR Apache-2.0
#include "AudioMixer.h"

#include "ClipAudioReader.h"

#include <algorithm>
#include <cmath>

namespace insv {

AudioMixer::AudioMixer(int sampleRate, int channels) : m_rate(sampleRate), m_channels(channels) {}

AudioMixer::~AudioMixer() = default;

void AudioMixer::setClips(std::vector<MixClip> clips)
{
    m_voices.clear();
    for (MixClip& c : clips) {
        if (c.lengthFrames <= 0 || c.gain <= 0.0f)
            continue;
        Voice v;
        v.clip = std::move(c);
        m_voices.push_back(std::move(v));
    }
}

void AudioMixer::mixInto(float* out, int64_t start, int64_t frames)
{
    const int64_t end = start + frames;
    const size_t ch = static_cast<size_t>(m_channels);
    for (Voice& v : m_voices) {
        const MixClip& c = v.clip;
        const int64_t clipEnd = c.startFrame + c.lengthFrames;
        if (clipEnd <= start) {
            v.reader.reset(); // played out: let go of the file until a seek back
            v.nextFrame = -1;
            continue;
        }
        const int64_t a = std::max(start, c.startFrame);
        const int64_t b = std::min(end, clipEnd);
        if (a >= b)
            continue;
        if (!v.reader) {
            v.reader = std::make_unique<ClipAudioReader>(m_rate, m_channels);
            v.reader->open(c.path, c.stream); // no audio => silence
            v.reader->setTempo(c.speed);
            v.reader->setNoiseReduction(c.noise);
            v.nextFrame = -1;
        }
        if (v.nextFrame != a) {
            v.reader->seek(c.inFrame + std::llround((a - c.startFrame) * c.speed));
            v.nextFrame = a;
        }
        const int64_t n = b - a;
        m_tmp.resize(static_cast<size_t>(n) * ch);
        v.reader->read(m_tmp.data(), n);
        v.nextFrame = b;

        float* dst = out + (a - start) * static_cast<int64_t>(ch);
        for (int64_t i = 0; i < n; ++i) {
            const int64_t local = a - c.startFrame + i; // frames into the clip
            float g = c.gain;
            if (local < c.fadeInFrames)
                g *= static_cast<float>(local) / static_cast<float>(c.fadeInFrames);
            const int64_t left = c.lengthFrames - local; // frames to the clip's end
            if (left <= c.fadeOutFrames && c.fadeOutFrames > 0)
                g *= static_cast<float>(left - 1) / static_cast<float>(c.fadeOutFrames);
            for (size_t k = 0; k < ch; ++k)
                dst[static_cast<size_t>(i) * ch + k] += m_tmp[static_cast<size_t>(i) * ch + k] * g;
        }
    }
}

void AudioMixer::limit(float* pcm, int64_t samples)
{
    constexpr float kT = 0.891251f; // -1 dBFS
    constexpr float kRoom = 1.0f - kT;
    for (int64_t i = 0; i < samples; ++i) {
        const float x = pcm[i];
        const float ax = std::abs(x);
        if (ax > kT)
            pcm[i] = std::copysign(std::min(1.0f, kT + kRoom * std::tanh((ax - kT) / kRoom)), x);
    }
}

} // namespace insv
