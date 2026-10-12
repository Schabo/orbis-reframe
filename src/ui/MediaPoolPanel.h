#pragma once

#include <QHash>
#include <QImage>
#include <QSet>
#include <QThreadPool>
#include <QTreeWidget>
#include <QWidget>

#include <functional>

#include "../core/MediaPool.h"

class QAction;
class QLabel;

// The media pool's list: a header row per folder, the folder's files under
// it. Dragging items out hands their files over as file URLs (what the
// timeline takes from a file manager); files and folders dropped in from
// outside are reported, not added (MainWindow probes them first).
class MediaPoolList : public QTreeWidget {
    Q_OBJECT

public:
    explicit MediaPoolList(QWidget *parent = nullptr);

signals:
    void filesDropped(const QStringList &paths);

protected:
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override;
    Qt::DropActions supportedDropActions() const override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    // Local files and folders among a drag's URLs; empty for a drag from the
    // list itself.
    QStringList droppedPaths(const QDropEvent *e) const;
};

// The left sidebar's Media tab: the project's media pool (Project's
// MediaPool), as a list grouped by the folder each file is in, sorted by
// name. Each file shows a thumbnail, its name, kind and length, size and
// frame rate; files used on the timeline are marked, missing files flagged.
// Import buttons; drag a file onto the timeline; double-click or Enter adds
// it there; right-click: add to timeline, show in the file manager, remove.
// Thumbnails come from a provider run on worker threads (the app's decodes
// a frame; none in tests).
class MediaPoolPanel : public QWidget {
    Q_OBJECT

public:
    // Item data of a row.
    enum Role {
        PathRole = Qt::UserRole + 1, // QString: the file; the folder for a folder row
        FolderRole,                  // bool: a folder's header row
        KindRole,                    // int: MediaItem::Kind
        DetailsRole,                 // QString: "360° · 0:42"
        FormatRole,                  // QString: "5.7K · 30 fps"
        InUseRole,                   // bool: on the timeline
        MissingRole,                 // bool: the file isn't there
        ThumbnailRole,               // QImage
        CountRole,                   // int: a folder's file count
    };

    using ThumbnailProvider = std::function<QImage(const MediaItem &, const QSize &)>;

    explicit MediaPoolPanel(MediaPool *pool, QWidget *parent = nullptr);
    ~MediaPoolPanel() override;

    void setThumbnailProvider(ThumbnailProvider provider);
    // The files the timeline uses (marked in the list; can't be removed).
    void setInUse(const QSet<QString> &paths);
    // A line under the header, e.g. "Adding 3 files…"; empty hides it.
    void setStatus(const QString &text);

    MediaPoolList *list() const { return m_list; }
    // The selected files (folder rows left out), in list order.
    QStringList selectedPaths() const;
    void selectPaths(const QStringList &paths);

    // Thumbnail size, in device-independent pixels.
    static QSize thumbnailSize();

signals:
    void importFilesRequested();
    void importFolderRequested();
    // Files / folders dropped in from outside.
    void filesDropped(const QStringList &paths);
    void addToTimelineRequested(const QStringList &paths);

private:
    void rebuild();
    void updateActions();
    void requestThumbnail(const MediaItem &m);
    void applyThumbnail(const QString &path, const QImage &image);
    void removeSelected();
    void showInFileManager();

    MediaPool *m_pool;
    MediaPoolList *m_list = nullptr;
    QLabel *m_status = nullptr;
    QAction *m_addAction = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_removeAction = nullptr;
    ThumbnailProvider m_thumbnailProvider;
    QSet<QString> m_inUse;
    QSet<QString> m_collapsed; // folders closed by the user
    QHash<QString, QImage> m_thumbnails; // by path; null = none to be had
    QThreadPool m_thumbnailPool;
};
