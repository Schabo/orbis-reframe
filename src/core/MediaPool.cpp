#include "MediaPool.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace media {

const QStringList &extensions(MediaItem::Kind kind)
{
    // 360 clips: the master, not the .lrv proxy.
    static const QStringList clip360 = {QStringLiteral("insv")};
    static const QStringList video = {QStringLiteral("mp4"), QStringLiteral("mov"),
                                      QStringLiteral("mkv"), QStringLiteral("m4v"),
                                      QStringLiteral("webm"), QStringLiteral("avi")};
    static const QStringList audio = {QStringLiteral("mp3"),  QStringLiteral("wav"),
                                      QStringLiteral("flac"), QStringLiteral("aac"),
                                      QStringLiteral("m4a"),  QStringLiteral("ogg"),
                                      QStringLiteral("opus")};
    switch (kind) {
    case MediaItem::Kind::Clip360:
        return clip360;
    case MediaItem::Kind::Video:
        return video;
    case MediaItem::Kind::Audio:
        break;
    }
    return audio;
}

std::optional<MediaItem::Kind> kindForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    for (const auto kind :
         {MediaItem::Kind::Clip360, MediaItem::Kind::Video, MediaItem::Kind::Audio})
        if (extensions(kind).contains(suffix))
            return kind;
    return std::nullopt;
}

QStringList filesInFolder(const QString &folder)
{
    QStringList out;
    const QDir dir(folder);
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name))
        if (kindForPath(fi.fileName()))
            out << fi.absoluteFilePath();
    return out;
}

QString firstMissingFile(const MediaItem &item)
{
    for (const QString &f : item.files())
        if (!QFileInfo::exists(f))
            return f;
    return QString();
}

void checkParts(MediaItem &item)
{
    for (MediaItem::Part &p : item.parts)
        p.missing = p.path != item.path && !QFileInfo::exists(p.path);
}

} // namespace media

QStringList MediaItem::partFiles() const
{
    QStringList out;
    for (const Part &p : parts)
        if (p.path != path)
            out << p.path;
    return out;
}

QList<SourceGap> MediaItem::gaps() const
{
    QList<SourceGap> out;
    qint64 end = 0; // where the part before ends
    for (const Part &p : parts) {
        if (p.startMs > end)
            out.append({end, p.startMs, QString()});
        if (p.missing)
            out.append({p.startMs, p.startMs + p.durationMs, p.path});
        end = std::max(end, p.startMs + p.durationMs);
    }
    return out;
}

MediaPool::MediaPool(QObject *parent) : QObject(parent) {}

int MediaPool::indexOf(const QString &path) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].path == path)
            return i;
    return -1;
}

const MediaItem *MediaPool::item(const QString &path) const
{
    const int i = indexOf(path);
    return i >= 0 ? &m_items[i] : nullptr;
}

bool MediaPool::add(MediaItem item)
{
    if (item.path.isEmpty() || contains(item.path))
        return false;
    item.missing = !media::firstMissingFile(item).isEmpty();
    media::checkParts(item);
    m_items.append(std::move(item));
    emit changed();
    return true;
}

void MediaPool::remove(const QStringList &paths)
{
    const auto removed = m_items.removeIf([&](const MediaItem &m) { return paths.contains(m.path); });
    if (removed > 0)
        emit changed();
}

bool MediaPool::relink(const QString &from, MediaItem item)
{
    const int i = indexOf(from);
    if (i < 0 || item.path.isEmpty())
        return false;
    if (item.path == from || contains(item.path)) {
        if (item.path != from) {
            m_items.removeAt(i);
        } else { // the same clip file; its take's other files may have moved
            m_items[i].companions = item.companions;
            m_items[i].parts = item.parts;
            m_items[i].missing = !media::firstMissingFile(m_items[i]).isEmpty();
            media::checkParts(m_items[i]);
        }
    } else {
        item.missing = !media::firstMissingFile(item).isEmpty();
        media::checkParts(item);
        m_items[i] = std::move(item);
    }
    emit changed();
    return true;
}

QStringList MediaPool::setMissing(const QHash<QString, bool> &missing)
{
    QStringList changed;
    for (MediaItem &m : m_items) {
        const auto it = missing.constFind(m.path);
        if (it != missing.cend() && m.missing != *it) {
            m.missing = *it;
            changed << m.path;
        }
        for (MediaItem::Part &p : m.parts) {
            const auto pit = missing.constFind(p.path);
            if (p.path != m.path && pit != missing.cend() && p.missing != *pit) {
                p.missing = *pit;
                if (!changed.contains(m.path))
                    changed << m.path;
            }
        }
    }
    if (!changed.isEmpty())
        emit missingChanged();
    return changed;
}

void MediaPool::setItems(const QList<MediaItem> &items)
{
    if (items == m_items)
        return;
    m_items = items;
    emit changed();
}
