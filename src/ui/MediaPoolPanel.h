#pragma once

#include <QHash>
#include <QImage>
#include <QListWidget>
#include <QSet>
#include <QThreadPool>
#include <QTreeWidget>
#include <QWidget>

#include <functional>

#include "DoubleClickToAddList.h"

#include "../core/MediaPool.h"

class QAction;
class QLabel;
class QStackedWidget;
class QTimer;
class QToolButton;

// The media pool's list: a header row per folder, the folder's files under
// it. Dragging items out hands their files over as file URLs (what the
// timeline takes from a file manager); files and folders dropped in from
// outside are reported, not added (MainWindow probes them first).
// Ctrl + wheel grows and shrinks the files' pictures (thumbScale).
class MediaPoolList : public QTreeWidget {
    Q_OBJECT

public:
    explicit MediaPoolList(QWidget *parent = nullptr);

    // The wheel zoom of the files' pictures: 1.0 is 80x45.
    double thumbScale() const { return m_thumbScale; }
    void setThumbScale(double scale);
    // A file row's picture size, and where it (and its play button) is in
    // the row, at the current zoom.
    QSize thumbSize() const;
    QRect thumbRect(const QRect &row) const;

signals:
    void filesDropped(const QStringList &paths);
    // A click on a file's picture (its play button).
    void previewClicked(const QString &path);
    // Space, with a file current (Qt's current item).
    void previewKeyPressed(const QString &path);
    // Return or Enter, with an item current: add it, as a double-click does.
    // A click itself can be an activation when the desktop asks for one
    // (KDE's single-click setting), which here would add a file the user meant
    // only to select: a click selects, and adding is a deliberate gesture.
    void activationKeyPressed();
    void thumbScaleChanged(double scale);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override;
    Qt::DropActions supportedDropActions() const override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    // The file Space previews: the current row's, if it's a file that's there.
    QString previewPath() const;

    double m_thumbScale = 1.0;
};

// The media pool's grid: every file as a cell with its picture on top and
// its name and kind underneath, all folders' files together in the list's
// folder-then-name order. The same drags, drops, preview and keys as the
// list: a click selects, and adding is a double-click or Return / Enter
// (DoubleClickToAddList), never Qt's activation. Ctrl + wheel grows and
// shrinks the cells' pictures (thumbScale).
class MediaPoolGrid : public DoubleClickToAddList {
    Q_OBJECT

public:
    explicit MediaPoolGrid(QWidget *parent = nullptr);

    // The wheel zoom of the cells' pictures: 1.0 is 160x90.
    double thumbScale() const { return m_thumbScale; }
    void setThumbScale(double scale);
    // A cell's picture size and the whole cell, at the current zoom.
    QSize thumbSize() const;
    QSize cellSize() const;
    // A cell's picture (and play button) inside the cell's rect.
    QRect thumbRect(const QRect &cell) const;

signals:
    void filesDropped(const QStringList &paths);
    void previewClicked(const QString &path);
    void previewKeyPressed(const QString &path);
    void thumbScaleChanged(double scale);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
    Qt::DropActions supportedDropActions() const override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    QString previewPath() const;

    double m_thumbScale = 1.0;
};

// The left sidebar's Media tab: the project's media pool (Project's
// MediaPool), as a list grouped by the folder each file is in, or as a grid
// of picture cells, sorted by name. Each file shows a thumbnail, its name,
// kind and length, size and frame rate; files used on the timeline are
// marked, missing files flagged. Import buttons; drag a file onto the
// timeline; double-click or Enter adds it there (a missing file: asks to
// locate it); right-click: add to timeline, locate a missing file, show in
// the file manager, remove. The play button on a file's picture, or Space,
// previews it (MainWindow plays it in the viewport instead of the timeline;
// setPreview marks it). Ctrl + wheel changes the pictures' size, per view.
// The cameras' <clip>_backup.wav files are hidden until the header's mic
// button shows them; a backup file on the timeline always shows.
// Thumbnails and a 360 clip's recording mode ("8K") come from providers run
// on worker threads (the app's decode a frame and ask the camera's reader;
// none in tests).
class MediaPoolPanel : public QWidget {
    Q_OBJECT

public:
    // Item data of a row or cell.
    enum Role {
        PathRole = Qt::UserRole + 1, // QString: the file; the folder for a folder row
        FolderRole,                  // bool: a folder's header row (list only)
        KindRole,                    // int: MediaItem::Kind
        DetailsRole,                 // QString: "360° · 0:42"
        FormatRole,                  // QString: "5.7K · 30 fps"
        InUseRole,                   // bool: on the timeline
        MissingRole,                 // bool: the file isn't there
        ThumbnailRole,               // QImage
        CountRole,                   // int: a folder's file count (list only)
        PreviewRole,                 // int: Preview, the file previewed (setPreview)
    };
    enum class Preview { None, Paused, Playing };
    enum class ViewMode { List, Grid };

    using ThumbnailProvider = std::function<QImage(const MediaItem &, const QSize &)>;
    // A 360 clip's recording mode as its camera names it ("5.7K"); empty when
    // the reader doesn't know it.
    using ModeProvider = std::function<QString(const MediaItem &)>;

    explicit MediaPoolPanel(MediaPool *pool, QWidget *parent = nullptr);
    ~MediaPoolPanel() override;

    void setThumbnailProvider(ThumbnailProvider provider);
    void setModeProvider(ModeProvider provider);
    // The files the timeline uses (marked in the list; can't be removed).
    void setInUse(const QSet<QString> &paths);
    // Read these files' thumbnails and modes again: a file replaced in
    // place, or back after it was gone.
    void forgetFiles(const QStringList &paths);
    // A line under the header, e.g. "Adding 3 files…"; empty hides it.
    void setStatus(const QString &text);
    // The file playing in the viewport instead of the timeline (empty:
    // none), and whether it plays: its play button stays up, as pause
    // while it plays.
    void setPreview(const QString &path, bool playing);

    // The view the files are shown in; the selection carries over.
    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);
    // The cameras' backup sound files (…_backup.wav beside their clip):
    // shown or hidden -- a file on the timeline always shows. Hidden out of
    // the box.
    bool backupAudioShown() const { return m_showBackupAudio; }
    void setBackupAudioShown(bool shown);
    // The views' wheel zooms, 1.0 the default; kept per view.
    double listScale() const;
    double gridScale() const;
    void setListScale(double scale);
    void setGridScale(double scale);

    MediaPoolList *list() const { return m_list; }
    MediaPoolGrid *grid() const { return m_grid; }
    // The selected files (folder rows left out), in the shown view's order.
    QStringList selectedPaths() const;
    void selectPaths(const QStringList &paths);

signals:
    void importFilesRequested();
    void importFolderRequested();
    // Files / folders dropped in from outside.
    void filesDropped(const QStringList &paths);
    void addToTimelineRequested(const QStringList &paths);
    // Locate file… on a missing file.
    void locateRequested(const QString &path);
    // Preview this file (its play button, Space), or play / pause it if
    // it's the one previewed.
    void previewRequested(const QString &path);
    // The view, the backup-audio filter and the views' zooms changed
    // (remembered per user).
    void viewModeChanged(ViewMode mode);
    void backupAudioShownChanged(bool shown);
    void listScaleChanged(double scale);
    void gridScaleChanged(double scale);

private:
    void rebuild();
    void updateActions();
    // The size thumbnails are wanted at, in device pixels: the bigger of
    // the views' pictures.
    QSize thumbnailRequestSize() const;
    void requestThumbnail(const MediaItem &m);
    // A view's zoom settled: the thumbnails again, if the views now want
    // them bigger than they were read.
    void requestThumbnails();
    void applyThumbnail(const QString &path, const QImage &image, int generation);
    void requestMode(const MediaItem &m);
    void applyMode(const QString &path, const QString &mode);
    void removeSelected();
    void showInFileManager();

    MediaPool *m_pool;
    MediaPoolList *m_list = nullptr;
    MediaPoolGrid *m_grid = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_viewList = nullptr;
    QToolButton *m_viewGrid = nullptr;
    QToolButton *m_backupAudio = nullptr;
    QAction *m_addAction = nullptr;
    QAction *m_locateAction = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_removeAction = nullptr;
    ThumbnailProvider m_thumbnailProvider;
    ModeProvider m_modeProvider;
    ViewMode m_viewMode = ViewMode::List;
    bool m_showBackupAudio = false;
    QSet<QString> m_inUse;
    QString m_previewPath;
    Preview m_preview = Preview::None;
    QSet<QString> m_collapsed; // folders closed by the user
    QHash<QString, QImage> m_thumbnails; // by path; null = none (yet) to be had
    // The size thumbnails are read at (device pixels). It only grows: the
    // views draw a bigger picture smaller, so zooming out reads nothing.
    QSize m_thumbnailReadSize;
    int m_thumbnailGeneration = 0; // bumped by each re-read at a new size
    QTimer *m_thumbnailTimer = nullptr; // the wheel settling
    QHash<QString, QString> m_modes;      // by path; empty = none to be had
    QThreadPool m_thumbnailPool;
};
