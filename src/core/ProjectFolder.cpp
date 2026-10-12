#include "ProjectFolder.h"

#include "Project.h"

#include <QDir>
#include <QFileInfo>

namespace projectfolder {

namespace {

QString subDir(const QString &projectFile, const char *name)
{
    if (!isProjectFolder(projectFile))
        return {};
    return QFileInfo(projectFile).absolutePath() + QLatin1Char('/') + QLatin1String(name);
}

} // namespace

QString projectFilePath(const QString &parentDir, const QString &name)
{
    return QDir::cleanPath(parentDir + QLatin1Char('/') + name + QLatin1Char('/') + name +
                           QLatin1Char('.') + QLatin1String(Project::kFileExtension));
}

QString nameProblem(const QString &name)
{
    if (name.trimmed().isEmpty())
        return QObject::tr("Give the project a name.");
    if (name != name.trimmed())
        return QObject::tr("The name can't start or end with a space.");
    if (name.contains(QLatin1Char('/')) || name.contains(QChar(0)))
        return QObject::tr("The name can't contain \"/\".");
    if (name == QLatin1String(".") || name == QLatin1String("..") || name.startsWith(QLatin1Char('.')))
        return QObject::tr("The name can't start with \".\".");
    // Ext4 and most others cap a file name at 255 bytes; leave room for ".orp".
    if (name.toUtf8().size() > 200)
        return QObject::tr("The name is too long.");
    return {};
}

QString create(const QString &parentDir, const QString &name, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (const QString problem = nameProblem(name); !problem.isEmpty())
        return fail(problem);
    if (parentDir.isEmpty())
        return fail(QObject::tr("Choose where to put the project."));
    const QString folder = QDir::cleanPath(parentDir + QLatin1Char('/') + name);
    if (QFileInfo::exists(folder))
        return fail(QObject::tr("%1 already exists. Choose another name or location.").arg(folder));
    QDir dir;
    for (const char *sub : {"cache", "exports", "screenshots"}) {
        if (!dir.mkpath(folder + QLatin1Char('/') + QLatin1String(sub)))
            return fail(QObject::tr("Can't create the folder %1.").arg(folder));
    }
    return projectFilePath(parentDir, name);
}

bool isProjectFolder(const QString &projectFile)
{
    if (projectFile.isEmpty())
        return false;
    const QFileInfo fi(projectFile);
    return fi.suffix().compare(QLatin1String(Project::kFileExtension), Qt::CaseInsensitive) == 0
           && fi.completeBaseName() == fi.absoluteDir().dirName();
}

QString cacheDir(const QString &projectFile) { return subDir(projectFile, "cache"); }
QString exportsDir(const QString &projectFile) { return subDir(projectFile, "exports"); }
QString screenshotsDir(const QString &projectFile) { return subDir(projectFile, "screenshots"); }

QString ensure(const QString &dir)
{
    if (dir.isEmpty() || !QDir().mkpath(dir))
        return {};
    return dir;
}

} // namespace projectfolder
