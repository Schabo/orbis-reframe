#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <optional>

#include "../core/MediaPool.h"

// Reading media files for the media pool. Both are safe on a worker thread.

// What `path` is (by its extension) and its length, size and frame rate, read
// from the file: an .insv through the clip probe, a flat video and an audio
// file through FFmpeg. Nothing, with *error set, when it can't be read as its
// kind.
std::optional<MediaItem> probeMedia(const QString &path, QString *error = nullptr);

// A picture for the Media tab, `size` (cropped to its shape): an .insv's
// stitched thumbnail from its trailer (clipThumbnail), a flat video's frame
// at one second (or its middle when shorter). Null for audio and on failure.
QImage mediaThumbnail(const MediaItem &item, const QSize &size);
