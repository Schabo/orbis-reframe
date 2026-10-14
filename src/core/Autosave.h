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
    // True when `projectFile` has a recovery file not older than itself: changes
    // that were never saved. (An older one is stale: a later save has them.)
    static bool hasRecovery(const QString &projectFile);

    // Write the recovery file now if there are unsaved changes not in it yet
    // (the timer calls this); remove it when there are none.
    void tick();
    // The unsaved changes were thrown away: remove the recovery file.
    void discard();

    QString writtenPath() const { return m_written; }

private:
    void removeWritten();

    Project *m_project;
    QTimer m_timer;
    QString m_written;    // the recovery file this session wrote, if any
    QByteArray m_content; // what's in it
};
