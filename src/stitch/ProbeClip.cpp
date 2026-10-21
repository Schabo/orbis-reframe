#include "ProbeClip.h"

#include <algorithm>

#include <ClipMetadata.h> // camera::ClipFileOf, ReadClipMetadata

namespace stitch {

bool ProbeClip(const std::string& clip_path, ClipProbe& out, std::string& err)
{
    const std::string file = camera::ClipFileOf(clip_path, &err);
    if (file.empty())
        return false;
    const auto clip = camera::ReadClipMetadata(file, &err);
    if (!clip)
        return false;
    out.clip_path = file;
    for (const camera::Lens& l : clip->lenses)
        if (l.source.path != file &&
            std::find(out.other_files.begin(), out.other_files.end(), l.source.path) ==
                out.other_files.end())
            out.other_files.push_back(l.source.path);
    out.width = clip->lensWidth;
    out.height = clip->lensHeight;
    out.fps = clip->fps;
    out.duration_ms = clip->durationMs;
    return true;
}

} // namespace stitch
