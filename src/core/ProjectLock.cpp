#include "ProjectLock.h"

#include <QFileInfo>
#include <QLockFile>
#include <QSysInfo>

#include <cstdio>

ProjectLock::ProjectLock(const QString &projectFile)
    : m_projectFile(QFileInfo(projectFile).absoluteFilePath()),
      m_lock(std::make_unique<QLockFile>(pathFor(m_projectFile)))
{
    m_lock->setStaleLockTime(0); // stale only when its process is gone
    if (m_lock->tryLock(0)) {
        m_state = State::Locked;
        return;
    }
    if (m_lock->error() == QLockFile::LockFailedError) {
        m_state = State::HeldElsewhere;
        QString host, app;
        if (m_lock->getLockInfo(&m_holderPid, &host, &app) &&
            host != QSysInfo::machineHostName())
            m_holderHost = host;
        return;
    }
    std::fprintf(stderr, "[project] can't make the lock file %s; the project opens without one\n",
                 qPrintable(pathFor(m_projectFile)));
}

ProjectLock::~ProjectLock() = default; // QLockFile unlocks

QString ProjectLock::pathFor(const QString &projectFile)
{
    return projectFile + QStringLiteral(".lock");
}
