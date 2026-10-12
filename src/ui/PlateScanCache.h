#pragma once

// The privacy blur's plate scans, one per source file (privacy::PlateScan),
// kept on disk in a cache folder -- keyed by the file's path, size and
// modification time, like WaveformCache -- and in memory once read. The
// folder is the open project's own cache/ (projectfolder::cacheDir), so a
// deleted project folder takes its scans with it; the user cache for an
// untitled or loose project. Every clip cut from one file uses its scan.
// GUI thread only.

#include <PlateScanData.h>

#include <QHash>
#include <QString>

#include <memory>

class PlateScanCache {
public:
    using Scan = std::shared_ptr<const privacy::PlateScan>;

    // The scan of `source`, read from disk on first ask; null when there is
    // none (or the file changed since).
    Scan get(const QString &source);
    // Fold `scan` into what `source` has (privacy::PlateScan::merge) and
    // write it. False with *error when it can't be written (the merged scan
    // is still served from memory).
    bool store(const QString &source, const privacy::PlateScan &scan, QString *error = nullptr);

    // `from` was moved or renamed to `to`: give `to` the scan `from` had, if
    // there is one on disk and `to` has none. Found by `from`'s path with
    // `to`'s size and modification time, which a move keeps (a copy may
    // not: then the clip is scanned again). True if one was carried over.
    bool carryOver(const QString &from, const QString &to);

    // The folder scans are kept in (plates/ is added); empty = the user
    // cache. A change forgets what was read from the old one.
    void setDirectory(const QString &dir);
    QString diskPath(const QString &source) const { return diskPathIn(m_dir, source); }
    static QString diskPathIn(const QString &dir, const QString &source);

private:
    static QString diskPathIn(const QString &dir, const QString &absPath, qint64 size,
                              qint64 mtimeMs);

    QString m_dir;
    QHash<QString, Scan> m_scans; // a null value = looked, there is none
};
