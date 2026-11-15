// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/audio/NoiseFilter.h>
#include <orbis-core/audio/GeneralWindReducer.h>
#include <orbis-core/audio/HissHumReducer.h>
#ifdef ORBISCORE_HAVE_VOICE
#include <orbis-core/audio/VoiceIsolator.h>
#endif
#include <orbis-core/audio/WindReducer.h>

namespace orbiscore {

std::unique_ptr<NoiseFilter> makeNoiseFilter(NoiseMode mode, int sampleRate, int channels)
{
    switch (mode) {
    case NoiseMode::Off:
        return nullptr;
    case NoiseMode::MotorcycleWind:
        return std::make_unique<WindReducer>(sampleRate, channels);
    case NoiseMode::Wind:
        return std::make_unique<GeneralWindReducer>(sampleRate, channels);
    case NoiseMode::Hiss:
        return std::make_unique<HissHumReducer>(sampleRate, channels);
    case NoiseMode::Voice:
    case NoiseMode::VoiceAmbience:
#ifdef ORBISCORE_HAVE_VOICE
        return std::make_unique<VoiceIsolator>(sampleRate, channels,
                                               mode == NoiseMode::Voice ? VoiceIsolator::Kind::VoiceOnly
                                                                        : VoiceIsolator::Kind::VoiceAmbience);
#else
        return nullptr; // built without ONNX Runtime: the voice modes do nothing
#endif
    }
    return nullptr;
}

bool voiceModesAvailable(std::string* why)
{
#ifdef ORBISCORE_HAVE_VOICE
    return VoiceIsolator::available(why);
#else
    if (why)
        *why = "built without ONNX Runtime (libonnxruntime-dev)";
    return false;
#endif
}

bool voiceModesReady(std::string* why)
{
#ifdef ORBISCORE_HAVE_VOICE
    return VoiceIsolator::ready(why);
#else
    return voiceModesAvailable(why);
#endif
}

} // namespace orbiscore
