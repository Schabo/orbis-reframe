#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

#include "MediaProbe.h"        // ClipProbeFn
#include "../core/MediaPool.h" // MediaItem

class MediaPoolPanel;
class Project;
class TabSidebar;
class QWidget;

// Getting files into the media pool (the left sidebar's Media tab) and
// finding them again: Import files… / Import folder… / a drop, probed on
// worker threads with the tab's status line counting down; and Locate file…
// for a missing one. Talks to the user through `window`'s dialogs.
class MediaImporter : public QObject {
    Q_OBJECT

public:
    MediaImporter(Project *project, MediaPoolPanel *panel, TabSidebar *sidebar,
                  ClipProbeFn probe, QWidget *window);

    // Pick files / a folder, then import().
    void importFiles();
    void importFolder();
    // Probe `paths` (a folder: the files in it) on worker threads and add the
    // ones that can be read to the pool; the ones that can't are reported
    // when the last is done. Files already in the pool are selected instead.
    void import(const QStringList &paths);
    // Another project (or the library) is shown: imports still being probed
    // for the one before are dropped when they land.
    void newSession() { ++m_session; }

    // Locate file… on the missing pool file `path`: pick where it is now; it
    // must be the same file (kind and length). Other missing files whose
    // names are in the picked file's folder come along. Returns them as
    // (old path, its item now), the picked one first; empty when nothing
    // was picked or it wasn't the same file (the user has been told why).
    using Relinks = QList<std::pair<QString, MediaItem>>;
    Relinks locate(const QString &path);
    // After the caller relinked them: select the picked file in the tab, and
    // say which other files were found with it.
    void showRelinked(const Relinks &relinks);

    // Copy the files the project uses into <project>/Media -- the ones on a
    // connected device (mounted volumes under the media roots, or one folder
    // the user picks), or missing and found there. Runs in the background
    // with a progress dialog; the copies' pool items come back through
    // relinksReady for the caller to relink. One at a time.
    void collectIntoProject();

signals:
    // (old path, the copy's probed item) for each file copied into the
    // project folder -- same shape locate() returns.
    void relinksReady(const Relinks &relinks);

private:
    // `file` probed, if it is the same file as `was`; else *why says how not.
    std::optional<MediaItem> probeSame(const MediaItem &was, const QString &file,
                                       QString *why) const;

    Project *m_project;
    MediaPoolPanel *m_panel;
    TabSidebar *m_sidebar;
    ClipProbeFn m_probe;
    QWidget *m_window;
    QString m_lastDir;       // where files were last imported from
    int m_pending = 0;       // files still being probed
    QStringList m_failures;  // "name: why", reported when the last is done
    int m_session = 0;
    bool m_collecting = false; // a copy into the project folder is running
};
