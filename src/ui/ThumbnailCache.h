#pragma once

#include <QImage>
#include <QMutex>
#include <QSize>
#include <QString>

#include <functional>

#include "../core/MediaPool.h"

// The Media tab's thumbnails, kept on disk so reopening a project doesn't
// decode every flat video again: a PNG per file and size, keyed by the file's
// path, size and modification time like WaveformCache (a changed file gets a
// new one). The folder is the open project's cache/, or the user cache for an
// untitled or loose project. get() is safe on worker threads.
class ThumbnailCache {
public:
    using Provider = std::function<QImage(const MediaItem &, const QSize &)>;

    // The thumbnail of `item` at `size`: read from disk, else made by
    // `make` and written. Null when `make` has none (audio, a file that
    // can't be read); that isn't kept, so it's tried again next time.
    QImage get(const MediaItem &item, const QSize &size, const Provider &make) const;

    // The folder thumbnails are kept in (thumbnails/ is added); empty = the
    // user cache.
    void setDirectory(const QString &dir);
    QString diskPath(const QString &path, const QSize &size) const;

private:
    mutable QMutex m_mutex; // guards m_dir
    QString m_dir;
};
