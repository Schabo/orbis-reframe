#include "ClipBackend.h"

#include <orbis-core/audio/AudioDemux.h>     // orbiscore::AudioDemux::countAudioStreams
#include <NativeStitchSource.h> // stitch::MakeStitchSource

ClipBackend ClipBackend::real()
{
    ClipBackend b;
    b.probe = &stitch::ProbeClip;
    b.audioStreamCount = &orbiscore::AudioDemux::countAudioStreams;
    b.makeSource = &stitch::MakeStitchSource;
    return b;
}
