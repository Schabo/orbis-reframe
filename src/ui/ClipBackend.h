#pragma once

#include <IStitchSource.h> // stitch::IStitchSource
#include <ProbeClip.h>     // stitch::ClipProbe

#include <functional>
#include <memory>
#include <string>

// How MainWindow reads 360 clips: the clip probe (adding a clip, importing it
// into the media pool), the count of its audio streams, the stitch source
// playback opens, and whether the per-clip sync / seam measurements run (they
// open the GPU stitcher themselves). real() is the app's; tests swap in
// stand-ins so the whole window runs offscreen without a GPU or camera files.
struct ClipBackend {
    std::function<bool(const std::string &path, stitch::ClipProbe &out, std::string &err)> probe;
    std::function<int(const std::string &path)> audioStreamCount;
    std::function<std::unique_ptr<stitch::IStitchSource>()> makeSource;
    bool measure = true;

    static ClipBackend real();
};
