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
    // In ms each part ends where the next starts, as in µs: its length is
    // its end's ms less its start's, not its µs length rounded on its own
    // (a gap of a ms in between would read as a part not found).
    if (clip->parts.size() > 1)
        for (const camera::Part& p : clip->parts)
            out.parts.push_back({p.path, p.startUs / 1000,
                                 (p.startUs + p.durationUs) / 1000 - p.startUs / 1000});
    out.width = clip->lensWidth;
    out.height = clip->lensHeight;
    out.fps = clip->fps;
    out.duration_ms = clip->durationMs;
    out.lens_guards = {clip->standardGuard.has_value(), clip->premiumGuard.has_value(),
                       clip->guardFitted};
    out.highlight_ms = clip->highlightMs;
    out.untested_model = clip->untestedModel ? clip->model : std::string();
    out.camera_stitched = clip->projection == camera::Projection::EquiAngularCube;
    return true;
}

} // namespace stitch
