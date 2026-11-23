#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <functional>
#include <optional>
#include <string>

#include <ProbeClip.h> // stitch::ClipProbe

#include "../core/MediaPool.h"

// Reading media files for the media pool. All are safe on a worker thread.

// What `path` is (by its extension) and its length, size and frame rate, read
// from the file: an .insv through the clip probe, a flat video and an audio
// file through FFmpeg. Nothing, with *error set, when it can't be read as its
// kind. `probeClip` reads an .insv (ClipBackend::probe); empty =
// stitch::ProbeClip. The item's path is the clip's file: for another file of
// a take (a ONE X2's `_10_` lens file or its proxy) the take's `_00_` file.
// `clipProbe` (optional): a 360 clip's probe, for what else it read (the
// lens-guard profiles, the highlight marks).
using ClipProbeFn = std::function<bool(const std::string &, stitch::ClipProbe &, std::string &)>;
std::optional<MediaItem> probeMedia(const QString &path, QString *error = nullptr,
                                    const ClipProbeFn &probeClip = {},
                                    stitch::ClipProbe *clipProbe = nullptr);

// The media pool's item for the 360 clip at `path` (absolute), read by the
// clip probe as `probe`: named by its take's file (probe.clip_path) when
// `path` is another file of the take, with the take's other files and a
// split recording's parts. What probeMedia gives for an .insv; for a file
// the clip probe reads whatever its name (a path from the command line).
MediaItem clipMediaItem(const QString &path, const stitch::ClipProbe &probe);

// A picture for the Media tab, `size` (cropped to its shape): a 360 clip's
// own preview from its trailer (clipThumbnail), a flat video's frame
// at one second (or its middle when shorter). Null for audio and on failure.
QImage mediaThumbnail(const MediaItem &item, const QSize &size);

// A 360 clip's recording mode as its camera's reader names it ("8K", "6K"),
// as the Media information panel shows it; empty for other kinds, for a size
// the camera has no name for, and on failure.
QString mediaMode(const MediaItem &item);
