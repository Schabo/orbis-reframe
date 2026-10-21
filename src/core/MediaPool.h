#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

// One file in the project's media pool. The source's metadata is stored with
// the project (like a clip's) so a project opens without probing its media.
struct MediaItem {
    enum class Kind {
        Clip360, // an .insv: for the main video track
        Video,   // a flat video: for an overlay track
        Audio,   // an audio file: for an audio track
    };

    QString path; // absolute
    Kind kind = Kind::Clip360;
    qint64 durationMs = 0;
    int width = 0; // video only
    int height = 0;
    double fps = 0.0; // 0 = not known
    // The other files of a take that are part of this clip (absolute): a
    // ONE X2 clip's lens-1 file. `path` names the clip; it can't play
    // without these. Empty for a clip in one file.
    QStringList companions;
    // The file -- or for a take, one of its files -- wasn't found when the
    // item was added, the project loaded, or the files were last checked
    // (Project::recheckMissingMedia). Not saved.
    bool missing = false;

    // `path`, then the companions.
    QStringList files() const { return QStringList{path} + companions; }

    bool operator==(const MediaItem &o) const
    {
        return path == o.path && kind == o.kind && durationMs == o.durationMs &&
               width == o.width && height == o.height && fps == o.fps &&
               companions == o.companions && missing == o.missing;
    }
};

namespace media {

// What a file is by its extension: .insv, a flat video (mp4, mov, ...) or an
// audio file (mp3, wav, ...). Nothing for anything else, the .lrv proxy
// included. The timeline's file drops and the pool's imports use the same
// lists.
std::optional<MediaItem::Kind> kindForPath(const QString &path);

// The extensions of one kind, lower case, no dot.
const QStringList &extensions(MediaItem::Kind kind);

// The files of a folder (not its subfolders) that kindForPath() knows, sorted
// by name.
QStringList filesInFolder(const QString &folder);

// The first of `item`'s files that isn't on the disk, empty when all are.
QString firstMissingFile(const MediaItem &item);

} // namespace media

// The project's media: every file imported into it, plus every file used on
// the timeline (Project adds those). Files are referenced where they are, not
// copied. Order is the order they were added; the Media tab sorts for display.
class MediaPool : public QObject {
    Q_OBJECT

public:
    explicit MediaPool(QObject *parent = nullptr);

    const QList<MediaItem> &items() const { return m_items; }
    int count() const { return int(m_items.size()); }
    bool contains(const QString &path) const { return indexOf(path) >= 0; }
    int indexOf(const QString &path) const;
    // Null if `path` isn't in the pool.
    const MediaItem *item(const QString &path) const;

    // Add `item` (its `missing` is set from the disk here). False, and
    // nothing changes, if its path is already in the pool.
    bool add(MediaItem item);
    // Remove these paths; the ones not in the pool are ignored.
    void remove(const QStringList &paths);
    // The file at `from` is `item` now (found again after a move or rename):
    // it takes `from`'s place in the list, or `from` just goes when `item`'s
    // path is in the pool already. `missing` is set from the disk. False if
    // `from` isn't in the pool.
    bool relink(const QString &from, MediaItem item);
    // Set `missing` from `missing` (path -> missing) for the items it names.
    // Emits missingChanged(), not changed(): a file coming and going isn't an
    // edit. Returns the paths that changed.
    QStringList setMissing(const QHash<QString, bool> &missing);
    // Replace everything (a project load / undo). Emits changed() once.
    void setItems(const QList<MediaItem> &items);
    void clear() { setItems({}); }

signals:
    // Anything in the pool changed.
    void changed();
    // Only items' `missing` changed (setMissing).
    void missingChanged();

private:
    QList<MediaItem> m_items;
};
