#include "ClipBackend.h"

#include <InsvAudioDemux.h>     // insv::InsvAudioDemux::countAudioStreams
#include <NativeStitchSource.h> // stitch::MakeStitchSource

ClipBackend ClipBackend::real()
{
    ClipBackend b;
    b.probe = &stitch::ProbeClip;
    b.audioStreamCount = &insv::InsvAudioDemux::countAudioStreams;
    b.makeSource = &stitch::MakeStitchSource;
    return b;
}
