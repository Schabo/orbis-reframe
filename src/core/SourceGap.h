#pragma once

#include <QString>

// A stretch of a clip's source with no file to play it from: a part of a
// recording the camera split over several files that's gone, or a gap
// where a part was never found (camera::ClipMetadata::parts). Source ms,
// [fromMs, toMs). The clip plays it black and silent, and can't be
// exported over it.
struct SourceGap {
    qint64 fromMs = 0;
    qint64 toMs = 0;
    QString file; // the missing part's file (absolute); empty when not known by name

    bool operator==(const SourceGap &) const = default;
};
