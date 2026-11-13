#include "MediaPool.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

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

QList<highlight::Mark> MediaItem::shownMarks(const QList<qint64> &cameraMarks) const
{
    return highlight::shown(marks, hiddenMarks,
                            kind == Kind::Clip360 ? cameraMarks : QList<qint64>());
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
            MediaItem &to = m_items[indexOf(item.path)];
            to.marks = highlight::merged(to.marks, m_items[i].marks);
            to.hiddenMarks = highlight::merged(to.hiddenMarks, m_items[i].hiddenMarks);
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
        item.marks = m_items[i].marks;
        item.hiddenMarks = m_items[i].hiddenMarks;
        m_items[i] = std::move(item);
    }
    emit changed();
    return true;
}

namespace {

// A frame of the file: how near another a new mark may not be. A file
// whose rate isn't known takes 30 fps, as its preview plays it.
qint64 frameMs(const MediaItem &m)
{
    return std::max<qint64>(1, std::llround(1000.0 / (m.fps > 0.0 ? m.fps : 30.0)));
}

QList<qint64> cameraMarksOf(const MediaItem &m, const QList<qint64> &cameraMarks)
{
    return m.kind == MediaItem::Kind::Clip360 ? cameraMarks : QList<qint64>();
}

} // namespace

void MediaPool::addMark(const QString &path, qint64 sourceMs, const QList<qint64> &cameraMarks)
{
    const int i = indexOf(path);
    if (i < 0 || m_items[i].kind == MediaItem::Kind::Audio)
        return;
    MediaItem &m = m_items[i];
    if (highlight::add(m.marks, m.hiddenMarks, sourceMs, m.durationMs, frameMs(m),
                       cameraMarksOf(m, cameraMarks)))
        emit changed();
}

void MediaPool::removeMarkNear(const QString &path, qint64 sourceMs,
                               const QList<qint64> &cameraMarks)
{
    const int i = indexOf(path);
    if (i < 0 || m_items[i].kind == MediaItem::Kind::Audio)
        return;
    MediaItem &m = m_items[i];
    if (highlight::removeNear(m.marks, m.hiddenMarks, sourceMs, 0, m.durationMs,
                              cameraMarksOf(m, cameraMarks)))
        emit changed();
}

void MediaPool::removeMark(const QString &path, qint64 sourceMs)
{
    const int i = indexOf(path);
    if (i >= 0 && m_items[i].marks.removeOne(sourceMs))
        emit changed();
}

void MediaPool::setMarkHidden(const QString &path, qint64 sourceMs, bool hidden)
{
    const int i = indexOf(path);
    if (i >= 0 && m_items[i].kind == MediaItem::Kind::Clip360 &&
        highlight::setHidden(m_items[i].hiddenMarks, sourceMs, hidden))
        emit changed();
}

void MediaPool::showHiddenMarks(const QString &path)
{
    const int i = indexOf(path);
    if (i < 0 || m_items[i].hiddenMarks.isEmpty())
        return;
    m_items[i].hiddenMarks.clear();
    emit changed();
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
