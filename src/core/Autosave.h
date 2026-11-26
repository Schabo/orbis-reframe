#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

class Project;

// Autosave as a recovery file: every interval, while the project has unsaved
// changes, the project as it is now is written to <file>.orp.autosave next to
// the project file. The project file itself only changes on Save. The
// recovery file goes away when the changes are saved or discarded, and when
// another project is opened; one left behind means the app didn't close
// properly, and opening the project offers it (MainWindow::openProject).
// An untitled project has no file to put it beside and isn't autosaved.
class Autosave : public QObject {
    Q_OBJECT

public:
    static constexpr int kDefaultIntervalMs = 2 * 60 * 1000;

    explicit Autosave(Project *project, int intervalMs = kDefaultIntervalMs,
                      QObject *parent = nullptr);
    ~Autosave() override;

    // The recovery file of project file `projectFile`.
    static QString pathFor(const QString &projectFile);
    // True when `projectFile` has a recovery file of changes that were never
    // saved: made from the project as it's saved now (Project::recoveryBase).
    // One made from an earlier save is stale -- a later save has its changes;
    // one from an older version, which didn't say, goes by the times (one
    // older than the project file is stale).
    static bool hasRecovery(const QString &projectFile);

    // Write the recovery file now if there are unsaved changes not in it yet
    // (the timer calls this); remove it when there are none.
    void tick();
    // The unsaved changes were thrown away: remove the recovery file.
    void discard();

private:
    void removeWritten();

    Project *m_project;
    QTimer m_timer;
    QString m_written;    // the recovery file this session wrote, if any
    QByteArray m_content; // what's in it
};
