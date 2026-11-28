#pragma once

#include <QStringList>

#include <functional>
#include <memory>

#include "../core/MediaPool.h"

namespace camera {
struct ClipMetadata;
}

// The files a take loses when the user deletes it from the Media tab.
namespace mediaremoval {

// Asks for the camera's metadata of a file.
using MetadataReader =
    std::function<std::shared_ptr<const camera::ClipMetadata>(const QString &path)>;

// The camera's proxy files in `metadata` that are on the disk: the take's
// own and each part's, once each.
QStringList proxyFiles(const camera::ClipMetadata &metadata);

// The backup sound files on the disk beside `files`: an X6's
// "<stem>_backup.wav", as media::isBackupAudio() names them.
QStringList backupAudioFiles(const QStringList &files);

// Every file of `item`: its own files, its split parts, the camera's proxies
// in `metadata` (skipped when it is null), and the backup sound files beside
// them.
QStringList takeFiles(const MediaItem &item, const camera::ClipMetadata *metadata);

// The same, with the camera's metadata read for every item but audio through
// `readMetadata`. A file the reader doesn't know adds no proxies.
QStringList takeFiles(const MediaItem &item, const MetadataReader &readMetadata);

} // namespace mediaremoval
