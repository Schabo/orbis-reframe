#pragma once

// The privacy blur's plate scans (privacy::PlateScan), each of one source
// file under a key (ClipItem::plateScan: a clip's own, made by newKey() for
// every scan), kept on disk in a cache folder -- named by the file's path,
// size and modification time, like WaveformCache, and the key -- and in
// memory once read. The empty key is the one scan per file of the builds
// before scans were per clip. The folder is the open project's own cache/
// (projectfolder::cacheDir), so a deleted project folder takes its scans
// with it; the user cache for an untitled or loose project. GUI thread only.

#include <PlateScanData.h>

#include <QHash>
#include <QSet>
#include <QString>

#include <memory>

class PlateScanCache {
public:
    using Scan = std::shared_ptr<const privacy::PlateScan>;

    // A key no scan has yet.
    static QString newKey();

    // The scan `key` of `source`, read from disk on first ask; null when
    // there is none (or the file changed since).
    Scan get(const QString &source, const QString &key);
    // Fold `scan` into what `key` of `source` has (privacy::PlateScan::merge)
    // and write it. False with *error when it can't be written (the merged
    // scan is still served from memory).
    bool store(const QString &source, const QString &key, const privacy::PlateScan &scan,
               QString *error = nullptr);

    // `from` was moved or renamed to `to`: give `to` scan `key` `from` had,
    // if there is one on disk and `to` has none. Found by `from`'s path with
    // `to`'s size and modification time, which a move keeps (a copy may
    // not: then the clip is scanned again). True if one was carried over.
    bool carryOver(const QString &from, const QString &to, const QString &key);

    // Remove the scans in folder `dir` (as setDirectory's; not the user
    // cache, which other projects share) whose key isn't in `keep`: those of
    // earlier scans no clip or undo step names any more. The scans of the
    // builds before per-clip ones (no key) are left. How many went.
    static int prune(const QString &dir, const QSet<QString> &keep);

    // The folder scans are kept in (plates/ is added); empty = the user
    // cache. A change forgets what was read from the old one.
    void setDirectory(const QString &dir);
    QString diskPath(const QString &source, const QString &key) const
    {
        return diskPathIn(m_dir, source, key);
    }
    static QString diskPathIn(const QString &dir, const QString &source, const QString &key);

private:
    static QString diskPathIn(const QString &dir, const QString &absPath, qint64 size,
                              qint64 mtimeMs, const QString &key);
    static QString memoryKey(const QString &source, const QString &key)
    {
        return source + QLatin1Char('\n') + key;
    }

    QString m_dir;
    QHash<QString, Scan> m_scans; // by memoryKey; a null value = looked, there is none
};
