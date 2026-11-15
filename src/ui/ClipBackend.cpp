#include "ClipBackend.h"

#include <orbis-core/audio/InsvAudioDemux.h>     // orbiscore::InsvAudioDemux::countAudioStreams
#include <NativeStitchSource.h> // stitch::MakeStitchSource

ClipBackend ClipBackend::real()
{
    ClipBackend b;
    b.probe = &stitch::ProbeClip;
    b.audioStreamCount = &orbiscore::InsvAudioDemux::countAudioStreams;
    b.makeSource = &stitch::MakeStitchSource;
    return b;
}
