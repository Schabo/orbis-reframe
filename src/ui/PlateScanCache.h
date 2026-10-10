#pragma once

// The privacy blur's plate scans, one per source file (privacy::PlateScan),
// kept on disk under the cache location -- keyed by the file's path, size and
// modification time, like WaveformCache -- and in memory once read. A scan is
// a property of the footage, not of a project: every clip cut from the file,
// in any project, uses it. GUI thread only.

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

    static QString diskPath(const QString &source);

private:
    QHash<QString, Scan> m_scans; // a null value = looked, there is none
};
