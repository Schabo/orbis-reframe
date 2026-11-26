#pragma once

#include <QString>

#include <memory>

class QLockFile;

// One editor per project: the window that opens or saves a project file
// holds a lock file beside it, <file>.orp.lock, for as long as the project is
// open there, and another orbis-reframe doesn't open it. Two editors on one
// file would overwrite each other's saves and recovery file.
//
// The lock file names the process and the computer holding it. One left by
// a process that's gone (a crash) is taken over; age alone never makes it
// stale, since a project can stay open for days. Where no lock file can be
// made (a folder that isn't writable), the project opens without one, as
// before locks existed.
class ProjectLock {
public:
    // Try to lock `projectFile` (absolute or not); see state().
    explicit ProjectLock(const QString &projectFile);
    ~ProjectLock();
    ProjectLock(const ProjectLock &) = delete;
    ProjectLock &operator=(const ProjectLock &) = delete;

    enum class State {
        Locked,        // ours until this object goes
        HeldElsewhere, // another orbis-reframe has the project open
        Unavailable,   // no lock file could be made: nothing guards the project
    };
    State state() const { return m_state; }
    // The project file, absolute.
    const QString &projectFile() const { return m_projectFile; }
    // With HeldElsewhere: the process holding it (0 when the lock file
    // doesn't say), and its computer's name when that isn't this one (a
    // shared folder).
    qint64 holderPid() const { return m_holderPid; }
    const QString &holderHost() const { return m_holderHost; }

    // The lock file of project file `projectFile`.
    static QString pathFor(const QString &projectFile);

private:
    QString m_projectFile;
    std::unique_ptr<QLockFile> m_lock;
    State m_state = State::Unavailable;
    qint64 m_holderPid = 0;
    QString m_holderHost;
};
