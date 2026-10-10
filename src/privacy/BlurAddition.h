#pragma once

// Privacy blur, manual correction: a plate
// the user picked with "Blur a plate…" and the app followed from there
// (PlateScanner::follow) -- one region per frame. Stored on the clip
// (ClipItem::blurAdditions, the project file), not in the scan: it is the
// user's, and a rescan must not drop it. Applied whether or not "Blur number
// plates" (the scan) is on.
//
// Header-only, no dependencies, so the project model can hold it.

#include <cstdint>
#include <vector>

namespace privacy {

// How the picture a region was found in was stitched: with the capture-time
// (rolling-shutter / lens timing) correction or without -- playback and
// export use it whenever the stabilization is on (any mode but Off) -- or
// on a clip without a gyro track, which never gets it. The correction moves
// the picture up to ~0.5 deg in fast swings, so regions found one way can
// sit off the plates stitched the other way.
enum class StitchTiming : uint8_t { Uncorrected = 0, Corrected = 1, NoGyro = 2 };

// Whether regions found with `found` sit off the picture stitched with the
// correction `correctedNow` (the stabilization on).
inline bool timingStale(StitchTiming found, bool correctedNow)
{
    return found != StitchTiming::NoGyro && (found == StitchTiming::Corrected) != correctedNow;
}

struct BlurAddition {
    struct Frame {
        int64_t tMs = 0; // source time of the frame
        // The region: corners TL, TR, BR, BL as unit directions in the
        // stitch's sphere frame (insv::BlurQuad's).
        float corner[4][3] = {};
        uint8_t kind = 0; // privacy::RegionKind, for the overlay
        bool operator==(const Frame &o) const
        {
            if (tMs != o.tMs || kind != o.kind)
                return false;
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i < 3; ++i)
                    if (corner[k][i] != o.corner[k][i])
                        return false;
            return true;
        }
    };
    std::vector<Frame> frames; // time order
    // The stitch it was followed in (before it was recorded: Corrected).
    StitchTiming timing = StitchTiming::Corrected;

    bool operator==(const BlurAddition &o) const { return frames == o.frames && timing == o.timing; }
};

} // namespace privacy
