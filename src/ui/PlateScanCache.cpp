#include "PlateScanCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

void PlateScanCache::setDirectory(const QString &dir)
{
    if (dir == m_dir)
        return;
    m_dir = dir;
    m_scans.clear();
}

QString PlateScanCache::diskPathIn(const QString &dir, const QString &source)
{
    const QFileInfo fi(source);
    return diskPathIn(dir, fi.absoluteFilePath(), fi.size(),
                      fi.lastModified().toMSecsSinceEpoch());
}

QString PlateScanCache::diskPathIn(const QString &dir, const QString &absPath, qint64 size,
                                   qint64 mtimeMs)
{
    const QByteArray id = (absPath + QLatin1Char('|') + QString::number(size) + QLatin1Char('|') +
                           QString::number(mtimeMs))
                              .toUtf8();
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex());
    const QString base =
        dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) : dir;
    return base + QStringLiteral("/plates/") + hash + QStringLiteral(".scan");
}

bool PlateScanCache::carryOver(const QString &from, const QString &to)
{
    const QFileInfo fi(to);
    const QString target = diskPath(to);
    if (!fi.exists() || QFileInfo::exists(target))
        return false;
    const QString old = diskPathIn(m_dir, QFileInfo(from).absoluteFilePath(), fi.size(),
                                   fi.lastModified().toMSecsSinceEpoch());
    if (!QFileInfo::exists(old))
        return false;
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (!QFile::copy(old, target))
        return false;
    m_scans.remove(to); // a "none" looked up before
    return true;
}

PlateScanCache::Scan PlateScanCache::get(const QString &source)
{
    if (source.isEmpty())
        return nullptr;
    if (const auto it = m_scans.constFind(source); it != m_scans.constEnd())
        return *it;
    Scan scan;
    auto s = std::make_shared<privacy::PlateScan>();
    std::string err;
    if (privacy::loadPlateScan(diskPath(source).toStdString(), *s, err))
        scan = std::move(s);
    m_scans.insert(source, scan);
    return scan;
}

bool PlateScanCache::store(const QString &source, const privacy::PlateScan &scan, QString *error)
{
    auto merged = std::make_shared<privacy::PlateScan>();
    if (const Scan old = get(source))
        *merged = *old;
    merged->merge(scan);
    m_scans.insert(source, merged);

    const QString path = diskPath(source);
    QDir().mkpath(QFileInfo(path).absolutePath());
    std::string err;
    if (!privacy::savePlateScan(*merged, path.toStdString(), err)) {
        if (error)
            *error = QString::fromStdString(err);
        return false;
    }
    return true;
}
