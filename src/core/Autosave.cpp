#include "Autosave.h"

#include "Project.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

Autosave::Autosave(Project *project, int intervalMs, QObject *parent)
    : QObject(parent), m_project(project)
{
    m_timer.setInterval(intervalMs);
    connect(&m_timer, &QTimer::timeout, this, &Autosave::tick);
    m_timer.start();
    // Saved, or back to the saved state: nothing to recover.
    connect(project, &Project::modifiedChanged, this, [this](bool modified) {
        if (!modified)
            removeWritten();
    });
    // Another project (opened, new, saved as): the old recovery file is
    // about the old one.
    connect(project, &Project::filePathChanged, this, [this] { removeWritten(); });
    // A measurement written into the project file: into the recovery file
    // too, so it has everything there is to recover.
    connect(project, &Project::measurementsWritten, this, [this] {
        if (m_written.isEmpty())
            return;
        m_content.clear();
        tick();
    });
}

Autosave::~Autosave() = default;

QString Autosave::pathFor(const QString &projectFile)
{
    return projectFile.isEmpty() ? QString() : projectFile + QStringLiteral(".autosave");
}

bool Autosave::hasRecovery(const QString &projectFile)
{
    const QFileInfo recovery(pathFor(projectFile));
    if (!recovery.isFile())
        return false;
    const QFileInfo project(projectFile);
    if (!project.exists())
        return true;
    QFile in(recovery.filePath());
    const QByteArray base = in.open(QIODevice::ReadOnly) ? Project::recoveryBase(in.readAll())
                                                         : QByteArray();
    if (!base.isEmpty()) {
        // Made from the project as it's saved now, or a save came after it.
        // A project file that can't be read or isn't a project doesn't say:
        // the recovery file is kept and offered.
        QFile saved(projectFile);
        if (!saved.open(QIODevice::ReadOnly))
            return true;
        const QByteArray now = Project::savedEditHash(saved.readAll());
        return now.isEmpty() || now == base;
    }
    // From a version that didn't say which save it was made from: by the
    // times. Not older: a measurement written into the project file is
    // followed at once by the recovery file, often within the same
    // millisecond.
    return recovery.lastModified() >= project.lastModified();
}

void Autosave::tick()
{
    const QString file = m_project->filePath();
    if (file.isEmpty() || !m_project->modified()) {
        removeWritten();
        return;
    }
    const QString target = pathFor(file);
    const QByteArray content = m_project->recoveryContents();
    if (target == m_written && content == m_content && QFileInfo::exists(target))
        return;
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text) || out.write(content) != content.size() ||
        !out.commit())
        return; // tried again next time; the project itself is untouched
    if (!m_written.isEmpty() && m_written != target)
        QFile::remove(m_written);
    m_written = target;
    m_content = content;
}

void Autosave::discard() { removeWritten(); }

void Autosave::removeWritten()
{
    if (!m_written.isEmpty())
        QFile::remove(m_written);
    m_written.clear();
    m_content.clear();
}
