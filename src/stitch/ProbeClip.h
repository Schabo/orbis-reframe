#pragma once

#include <cstdint>
#include <string>

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
};

bool ProbeClip(const std::string& clip_path, ClipProbe& out, std::string& err);

} // namespace stitch
