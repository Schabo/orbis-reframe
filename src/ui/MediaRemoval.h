#pragma once

#include <QStringList>

#include "../core/MediaPool.h"

namespace camera {
struct ClipMetadata;
}

// The files a take loses when the user deletes it from the Media tab.
namespace mediaremoval {

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

} // namespace mediaremoval
