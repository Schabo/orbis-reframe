#include "PlateScanCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

void PlateScanCache::setDirectory(const QString &dir)
{
    if (dir == m_dir)
        return;
    m_dir = dir;
    m_scans.clear();
}

QString PlateScanCache::newKey()
{
    return QUuid::createUuid().toString(QUuid::Id128).left(12);
}

QString PlateScanCache::diskPathIn(const QString &dir, const QString &source, const QString &key)
{
    const QFileInfo fi(source);
    return diskPathIn(dir, fi.absoluteFilePath(), fi.size(),
                      fi.lastModified().toMSecsSinceEpoch(), key);
}

QString PlateScanCache::diskPathIn(const QString &dir, const QString &absPath, qint64 size,
                                   qint64 mtimeMs, const QString &key)
{
    const QByteArray id = (absPath + QLatin1Char('|') + QString::number(size) + QLatin1Char('|') +
                           QString::number(mtimeMs))
                              .toUtf8();
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex());
    const QString base =
        dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) : dir;
    const QString suffix = key.isEmpty() ? QString() : QLatin1Char('-') + key;
    return base + QStringLiteral("/plates/") + hash + suffix + QStringLiteral(".scan");
}

int PlateScanCache::prune(const QString &dir, const QSet<QString> &keep)
{
    if (dir.isEmpty())
        return 0;
    // <40 hex digits of the file's hash>-<key>.scan
    static const QRegularExpression name(QStringLiteral("^[0-9a-f]{40}-(.+)\\.scan$"));
    QDir plates(dir + QStringLiteral("/plates"));
    int removed = 0;
    for (const QString &f : plates.entryList({QStringLiteral("*.scan")}, QDir::Files)) {
        const QRegularExpressionMatch m = name.match(f);
        if (m.hasMatch() && !keep.contains(m.captured(1)) && plates.remove(f))
            ++removed;
    }
    return removed;
}

bool PlateScanCache::carryOver(const QString &from, const QString &to, const QString &key)
{
    const QFileInfo fi(to);
    const QString target = diskPath(to, key);
    if (!fi.exists() || QFileInfo::exists(target))
        return false;
    const QString old = diskPathIn(m_dir, QFileInfo(from).absoluteFilePath(), fi.size(),
                                   fi.lastModified().toMSecsSinceEpoch(), key);
    if (!QFileInfo::exists(old))
        return false;
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (!QFile::copy(old, target))
        return false;
    m_scans.remove(memoryKey(to, key)); // a "none" looked up before
    return true;
}

PlateScanCache::Scan PlateScanCache::get(const QString &source, const QString &key)
{
    if (source.isEmpty())
        return nullptr;
    if (const auto it = m_scans.constFind(memoryKey(source, key)); it != m_scans.constEnd())
        return *it;
    Scan scan;
    auto s = std::make_shared<privacy::PlateScan>();
    std::string err;
    if (privacy::loadPlateScan(diskPath(source, key).toStdString(), *s, err))
        scan = std::move(s);
    m_scans.insert(memoryKey(source, key), scan);
    return scan;
}

bool PlateScanCache::store(const QString &source, const QString &key,
                           const privacy::PlateScan &scan, QString *error)
{
    auto merged = std::make_shared<privacy::PlateScan>();
    if (const Scan old = get(source, key))
        *merged = *old;
    merged->merge(scan);
    m_scans.insert(memoryKey(source, key), merged);

    const QString path = diskPath(source, key);
    QDir().mkpath(QFileInfo(path).absolutePath());
    std::string err;
    if (!privacy::savePlateScan(*merged, path.toStdString(), err)) {
        if (error)
            *error = QString::fromStdString(err);
        return false;
    }
    return true;
}
