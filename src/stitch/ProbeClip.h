#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "IStitchSource.h" // stitch::LensGuardProfiles

// Lightweight source-clip probe: native resolution / fps / duration, so a
// caller can work out the output frame count it must bake poses for (and fill
// ClipItem's source-media facts on import). From the clip's description
// (camera::ReadClipMetadata): no decode, no stitch.

namespace stitch {

struct ClipProbe {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t duration_ms = 0;
    // The file that names the clip: the one probed, or for another file of a
    // take (a lens's own file, the camera's proxy) the take's clip file
    // (camera::ClipFileOf). Empty = the one probed.
    std::string clip_path;
    // The take's other files the clip needs (a ONE X2 clip's lens-1 file);
    // empty for a clip in one file.
    std::vector<std::string> other_files;
    // A recording the camera split over several files
    // (camera::ClipMetadata::parts): every file in clip order, the clip's
    // first; empty for a clip in one file.
    struct Part {
        std::string path;
        int64_t start_ms = 0;
        int64_t duration_ms = 0;
    };
    std::vector<Part> parts;
    // The lens-guard profiles the clip's file carries.
    LensGuardProfiles lens_guards;
    // The highlight marks set while recording, video ms in time order
    // (camera::ClipMetadata::highlightMs).
    std::vector<int64_t> highlight_ms;
};

// Probes the clip `clip_path` is, or is part of.
bool ProbeClip(const std::string& clip_path, ClipProbe& out, std::string& err);

} // namespace stitch
