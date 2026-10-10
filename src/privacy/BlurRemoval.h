#pragma once

// Privacy blur, manual correction: a blur
// track the user removed from a clip -- e.g. a false hit on the rider's own
// jacket. Stored on the clip (ClipItem::blurRemovals, the project file), not in
// the scan: the scan is per source file and gets rescanned, and a rescan gives
// its tracks new ids. So a removal records the track's path on the sphere
// instead, and PlateScan::resolveRemovals() finds the track of the current scan
// that follows it.
//
// Header-only, no dependencies, so the project model can hold it.

#include <cstdint>
#include <vector>

namespace privacy {

struct BlurRemoval {
    // The source-time range removed, inclusive: the whole track, or from the
    // playhead to its end ("remove from here on").
    int64_t fromMs = 0;
    int64_t toMs = 0;
    // Removed from the track's start: a rescanned track that starts earlier
    // goes from its own start. Otherwise from fromMs. Either way to the
    // matched track's end.
    bool wholeTrack = true;
    // The track's centre over that range, a unit direction in the stitch's
    // sphere frame (insv::BlurQuad's), sampled every few frames.
    struct Point {
        int64_t tMs = 0;
        float dir[3] = {0, 0, 0};
        bool operator==(const Point &o) const
        {
            return tMs == o.tMs && dir[0] == o.dir[0] && dir[1] == o.dir[1] && dir[2] == o.dir[2];
        }
    };
    std::vector<Point> path;

    bool operator==(const BlurRemoval &o) const
    {
        return fromMs == o.fromMs && toMs == o.toMs && wholeTrack == o.wholeTrack && path == o.path;
    }
};

} // namespace privacy
