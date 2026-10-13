#include "ThumbnailCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>

QImage ThumbnailCache::get(const MediaItem &item, const QSize &size, const Provider &make) const
{
    if (size.isEmpty() || item.kind == MediaItem::Kind::Audio)
        return {};
    const QString disk = diskPath(item.path, size);
    if (!disk.isEmpty()) {
        QImage cached(disk);
        if (cached.size() == size)
            return cached;
    }
    QImage image = make ? make(item, size) : QImage();
    if (image.isNull() || disk.isEmpty())
        return image;
    QDir().mkpath(QFileInfo(disk).absolutePath());
    QSaveFile f(disk);
    if (f.open(QIODevice::WriteOnly) && image.save(&f, "PNG"))
        f.commit();
    return image;
}

void ThumbnailCache::setDirectory(const QString &dir)
{
    QMutexLocker lock(&m_mutex);
    m_dir = dir;
}

QString ThumbnailCache::diskPath(const QString &path, const QSize &size) const
{
    const QFileInfo fi(path);
    if (!fi.exists())
        return {};
    const QByteArray id =
        (fi.absoluteFilePath() + QLatin1Char('|') + QString::number(fi.size()) +
         QLatin1Char('|') + QString::number(fi.lastModified().toMSecsSinceEpoch()) +
         QLatin1Char('|') + QString::number(size.width()) + QLatin1Char('x') +
         QString::number(size.height()))
            .toUtf8();
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex());
    QString base;
    {
        QMutexLocker lock(&m_mutex);
        base = m_dir;
    }
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return base + QStringLiteral("/thumbnails/") + hash + QStringLiteral(".png");
}
