#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

#include "HighlightMarks.h"
#include "SourceGap.h"

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
    // A recording the camera split over several files: every file in clip
    // order, `path`'s first, where each starts in the clip and how long it
    // is (camera::ClipMetadata::parts). Kept so that a part gone later is
    // known, and where. A part starting after the one before ends leaves a
    // gap: a part that wasn't there when the clip was added. Empty for a
    // clip in one file. Unlike a companion, a part that's gone leaves the
    // rest of the clip playing.
    struct Part {
        QString path; // absolute
        qint64 startMs = 0;
        qint64 durationMs = 0;
        bool missing = false; // as `missing`, for this file; not saved
        bool operator==(const Part &) const = default;
    };
    QList<Part> parts;
    // The file -- or for a take, one of its files -- wasn't found when the
    // item was added, the project loaded, or the files were last checked
    // (Project::recheckMissingMedia). Not saved.
    bool missing = false;
    // Highlight marks set on the file in the Media tab's preview, in the
    // file's source ms (a split recording's across its parts), as a clip
    // keeps them (ClipItem::marks / hiddenMarks): the user's own, and the
    // camera's the user hid. A clip made from the file starts with a copy
    // of them; editing them later leaves clips already made as they are. A
    // flat video has only its own; an audio file none.
    QList<qint64> marks;
    QList<qint64> hiddenMarks;

    // `path`, then the companions.
    QStringList files() const { return QStringList{path} + companions; }
    // The later parts' files.
    QStringList partFiles() const;
    // The source spans no file plays: the parts that are missing, and the
    // gaps between parts, in source order.
    QList<SourceGap> gaps() const;
    // The marks the file shows, sorted: its own and those of the file's
    // `cameraMarks` it doesn't hide (a flat video's are ignored: it has
    // none).
    QList<highlight::Mark> shownMarks(const QList<qint64> &cameraMarks) const;

    bool operator==(const MediaItem &o) const
    {
        return path == o.path && kind == o.kind && durationMs == o.durationMs &&
               width == o.width && height == o.height && fps == o.fps &&
               companions == o.companions && parts == o.parts && missing == o.missing &&
               marks == o.marks && hiddenMarks == o.hiddenMarks;
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
// Its parts aren't asked: a missing one leaves the rest playing.
QString firstMissingFile(const MediaItem &item);

// Marks each of `item`'s parts missing or not by whether its file is on the disk.
void checkParts(MediaItem &item);

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
    // path is in the pool already. `missing` is set from the disk; the
    // highlight marks go with the file (into the item already there, with
    // its own). False if `from` isn't in the pool.
    bool relink(const QString &from, MediaItem item);
    // Set `missing` from `missing` (path -> missing) for the items it names,
    // and each part's for the parts it names. Emits missingChanged(), not
    // changed(): a file coming and going isn't an edit. Returns the items'
    // paths that changed (an item whose part changed among them).
    QStringList setMissing(const QHash<QString, bool> &missing);
    // The highlight marks of the file at `path` (MediaItem::marks), as the
    // timeline edits a clip's (Timeline::addClipMark and the rest), over
    // the whole file; `cameraMarks` are the file's. Refused on an audio
    // file, and the hiding on a flat video. Each emits changed() when it
    // changed something.
    void addMark(const QString &path, qint64 sourceMs, const QList<qint64> &cameraMarks);
    void removeMarkNear(const QString &path, qint64 sourceMs, const QList<qint64> &cameraMarks);
    void removeMark(const QString &path, qint64 sourceMs);
    void setMarkHidden(const QString &path, qint64 sourceMs, bool hidden);
    void showHiddenMarks(const QString &path);
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
