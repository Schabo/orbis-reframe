#include "MediaRemoval.h"

#include "ClipMetadata.h"

#include <QDir>
#include <QFileInfo>

namespace {

void addOnce(QStringList &files, const QString &path)
{
    if (!path.isEmpty() && !files.contains(path))
        files << path;
}

// Only a file that is there can be listed. A path from the camera's metadata
// may be stale.
void addExisting(QStringList &files, const QString &path)
{
    if (QFileInfo::exists(path))
        addOnce(files, path);
}

} // namespace

namespace mediaremoval {

QStringList proxyFiles(const camera::ClipMetadata &metadata)
{
    QStringList files;
    addExisting(files, QString::fromStdString(metadata.proxyPath));
    for (const camera::Part &part : metadata.parts)
        for (const camera::LensSource &lens : part.proxyLenses)
            addExisting(files, QString::fromStdString(lens.path));
    return files;
}

QStringList backupAudioFiles(const QStringList &files)
{
    QStringList backups;
    for (const QString &file : files) {
        const QFileInfo info(file);
        const QString candidate = info.absoluteDir().filePath(
            info.completeBaseName() + QStringLiteral("_backup.wav"));
        if (QFileInfo::exists(candidate) && media::isBackupAudio(candidate))
            addOnce(backups, candidate);
    }
    return backups;
}

QStringList takeFiles(const MediaItem &item, const camera::ClipMetadata *metadata)
{
    QStringList files;
    const QStringList own = item.files() + item.partFiles();
    for (const QString &file : own)
        addOnce(files, file);
    if (metadata)
        for (const QString &file : proxyFiles(*metadata))
            addOnce(files, file);
    for (const QString &file : backupAudioFiles(own))
        addOnce(files, file);
    return files;
}

} // namespace mediaremoval
