#include "ProbeClip.h"

#include <ClipMetadata.h> // camera::ReadClipMetadata

namespace stitch {

bool ProbeClip(const std::string& clip_path, ClipProbe& out, std::string& err)
{
    const auto clip = camera::ReadClipMetadata(clip_path, &err);
    if (!clip)
        return false;
    out.width = clip->lensWidth;
    out.height = clip->lensHeight;
    out.fps = clip->fps;
    out.duration_ms = clip->durationMs;
    return true;
}

} // namespace stitch
