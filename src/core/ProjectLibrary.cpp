#include "ProjectLibrary.h"

#include "Project.h"
#include "ProjectFolder.h"
#include "Timeline.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace {

constexpr const char *kFormatTag = "orbis-reframe-library";
constexpr int kFormatVersion = 1;

QString clean(const QString &path)
{
    return path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
}

} // namespace

ProjectLibrary::ProjectLibrary(const QString &listFile)
    : m_listFile(listFile), m_projectsFolder(standardProjectsFolder())
{
}

QString ProjectLibrary::defaultListFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/projects.json");
}

QString ProjectLibrary::standardProjectsFolder()
{
    QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (videos.isEmpty())
        videos = QDir::homePath() + QStringLiteral("/Videos");
    return videos + QStringLiteral("/orbis-reframe");
}

void ProjectLibrary::load()
{
    m_listed.clear();
    m_hidden.clear();
    m_projectsFolder = standardProjectsFolder();
    QFile f(m_listFile);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value(QLatin1String("format")).toString() != QLatin1String(kFormatTag))
        return;
    if (const QString folder = root.value(QLatin1String("projectsFolder")).toString(); !folder.isEmpty())
        m_projectsFolder = folder;
    for (const QJsonValue &v : root.value(QLatin1String("projects")).toArray()) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.path = clean(o.value(QLatin1String("path")).toString());
        e.lastOpened = QDateTime::fromString(o.value(QLatin1String("lastOpened")).toString(),
                                             Qt::ISODateWithMs);
        if (!e.path.isEmpty())
            m_listed.push_back(e);
    }
    for (const QJsonValue &v : root.value(QLatin1String("hidden")).toArray())
        if (const QString p = clean(v.toString()); !p.isEmpty())
            m_hidden.push_back(p);
    std::stable_sort(m_listed.begin(), m_listed.end(),
                     [](const Entry &a, const Entry &b) { return a.lastOpened > b.lastOpened; });
}

bool ProjectLibrary::save(QString *error) const
{
    QJsonArray projects;
    for (const Entry &e : m_listed)
        projects.append(QJsonObject{{QStringLiteral("path"), e.path},
                                    {QStringLiteral("lastOpened"),
                                     e.lastOpened.toString(Qt::ISODateWithMs)}});
    QJsonObject root{{QStringLiteral("format"), QLatin1String(kFormatTag)},
                     {QStringLiteral("version"), kFormatVersion},
                     {QStringLiteral("projects"), projects}};
    if (m_projectsFolder != standardProjectsFolder())
        root.insert(QStringLiteral("projectsFolder"), m_projectsFolder);
    if (!m_hidden.isEmpty())
        root.insert(QStringLiteral("hidden"), QJsonArray::fromStringList(m_hidden));

    QDir().mkpath(QFileInfo(m_listFile).absolutePath());
    QSaveFile f(m_listFile);
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(QJsonDocument(root).toJson()) < 0 || !f.commit()) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(m_listFile, f.errorString());
        return false;
    }
    return true;
}

bool ProjectLibrary::setProjectsFolder(const QString &folder, QString *error)
{
    load(); // what's on disk now (another window may have changed it)
    m_projectsFolder = folder.isEmpty() ? standardProjectsFolder() : clean(folder);
    return save(error);
}

QStringList ProjectLibrary::scanProjectsFolder() const
{
    QStringList found;
    const QDir dir(m_projectsFolder);
    for (const QString &sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString file = projectfolder::projectFilePath(m_projectsFolder, sub);
        if (QFileInfo(file).isFile())
            found.push_back(clean(file));
    }
    return found;
}

QList<ProjectLibrary::Entry> ProjectLibrary::entries() const
{
    QList<Entry> all;
    QSet<QString> seen;
    for (Entry e : m_listed) {
        e.sortTime = e.lastOpened;
        all.push_back(e);
        seen.insert(e.path);
    }
    for (const QString &path : scanProjectsFolder()) {
        if (seen.contains(path) || m_hidden.contains(path))
            continue;
        Entry e;
        e.path = path;
        e.sortTime = QFileInfo(path).lastModified();
        all.push_back(e);
    }
    std::stable_sort(all.begin(), all.end(),
                     [](const Entry &a, const Entry &b) { return a.sortTime > b.sortTime; });
    return all;
}

bool ProjectLibrary::noteOpened(const QString &path, const QDateTime &when, QString *error)
{
    const QString p = clean(path);
    if (p.isEmpty())
        return true;
    load();
    m_listed.removeIf([&](const Entry &e) { return e.path == p; });
    m_hidden.removeAll(p);
    m_listed.push_front({p, when, when});
    return save(error);
}

bool ProjectLibrary::remove(const QString &path, QString *error)
{
    const QString p = clean(path);
    load();
    m_listed.removeIf([&](const Entry &e) { return e.path == p; });
    if (scanProjectsFolder().contains(p) && !m_hidden.contains(p))
        m_hidden.push_back(p);
    return save(error);
}

ProjectSummary ProjectSummary::read(const QString &path)
{
    ProjectSummary s;
    s.path = path;
    s.name = QFileInfo(path).completeBaseName();
    if (!QFileInfo(path).isFile()) {
        s.fileMissing = true;
        return s;
    }
    Project project; // no parent: may live on a worker thread
    if (!project.load(path, &s.error))
        return s;
    const Timeline *t = project.timeline();
    s.clipCount = t->count();
    s.durationMs = t->totalDurationMs();
    if (t->count() > 0)
        s.firstClip = t->clipAt(0).sourcePath;
    QSet<QString> missing;
    for (int i = 0; i < t->count(); ++i)
        if (t->clipAt(i).mediaMissing)
            missing.insert(t->clipAt(i).sourcePath);
    for (int i = 0; i < t->trackCount(); ++i)
        for (const TrackClip &c : t->trackAt(i).clips)
            if (c.mediaMissing)
                missing.insert(c.sourcePath);
    s.missingMedia = int(missing.size());
    return s;
}
