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
// `lensGuards` (optional): an .insv's lens-guard profiles (ClipProbe's).
using ClipProbeFn = std::function<bool(const std::string &, stitch::ClipProbe &, std::string &)>;
std::optional<MediaItem> probeMedia(const QString &path, QString *error = nullptr,
                                    const ClipProbeFn &probeClip = {},
                                    stitch::LensGuardProfiles *lensGuards = nullptr);

// A picture for the Media tab, `size` (cropped to its shape): a 360 clip's
// own preview from its trailer (clipThumbnail), a flat video's frame
// at one second (or its middle when shorter). Null for audio and on failure.
QImage mediaThumbnail(const MediaItem &item, const QSize &size);

// A 360 clip's recording mode as its camera's reader names it ("8K", "6K"),
// as the Media information panel shows it; empty for other kinds, for a size
// the camera has no name for, and on failure.
QString mediaMode(const MediaItem &item);
