// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace orbiscore {

// Where in a stream's frame a lens's picture is, as fractions of the frame's
// width and height: the whole frame when the lens has a stream of its own, a
// half where one stream holds both lenses side by side (the cameras' proxies,
// e.g. docs/formats/insta360-x5.md §3.2).
struct LensRect {
    double x = 0.0;
    double y = 0.0;
    double w = 1.0;
    double h = 1.0;
};

// Where a lens's picture comes from: a file, its n-th video stream and the
// part of that stream's frame. One file can hold every lens (the X5: streams
// 0 and 1), each lens can have a file of its own (the ONE X2 at 5.7K), or one
// stream can hold both (a proxy). A cover picture stored as a video stream
// (flagged attached_pic or timed_thumbnails) isn't counted.
struct LensSource {
    std::string path;
    int stream = 0; // index among the file's video streams, covers left out
    LensRect rect;
};

// A stretch of a clip with files of its own: a recording longer than the
// camera's file limit is split over several files, which play back to back
// as one clip. `startUs` is where its first frame sits in the clip; every
// part has the same lenses, coded alike.
struct ClipPart {
    int64_t startUs = 0;
    std::vector<LensSource> lenses;
};

} // namespace orbiscore
