#include "ProjectFolder.h"

#include "Autosave.h"
#include "ColorLut.h"
#include "Project.h"
#include "ZipReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <memory>
#include <string>

namespace projectfolder {

namespace {

QString failWith(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return {};
}

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
QString lutsDir(const QString &projectFile) { return subDir(projectFile, "luts"); }

namespace {

// `bytes` written into `dir` as `fileName`, or as "<name> 2.cube", ... when a
// different file has that name; a file with the same bytes is reused.
QString writeLut(const QString &dir, const QString &fileName, const QByteArray &bytes, QString *error)
{
    if (!QDir().mkpath(dir))
        return failWith(error, QObject::tr("Can't create the folder %1.").arg(QDir::toNativeSeparators(dir)));
    const QFileInfo src(fileName);
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? fileName
                                    : QStringLiteral("%1 %2.%3")
                                          .arg(src.completeBaseName())
                                          .arg(n)
                                          .arg(src.suffix());
        const QString dest = dir + QLatin1Char('/') + name;
        QFile out(dest);
        if (out.exists()) {
            if (out.open(QIODevice::ReadOnly) && out.readAll() == bytes)
                return dest; // the same LUT, picked again
            continue;
        }
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.flush()) {
            out.remove();
            return failWith(error,
                            QObject::tr("Can't copy the LUT to %1.").arg(QDir::toNativeSeparators(dir)));
        }
        return dest;
    }
}

std::unique_ptr<zip::Archive> openZip(const QString &zipFile, QString *error)
{
    std::string why;
    auto archive = zip::Archive::open(zipFile.toStdString(), &why);
    if (!archive)
        failWith(error, QObject::tr("%1 can't be opened: %2.")
                            .arg(QFileInfo(zipFile).fileName(), QString::fromStdString(why)));
    return archive;
}

} // namespace

QString importLut(const QString &projectFile, const QString &lutFile, QString *error)
{
    const QString dir = lutsDir(projectFile);
    const QFileInfo src(lutFile);
    if (dir.isEmpty())
        return src.absoluteFilePath();
    if (src.absolutePath() == QFileInfo(dir).absoluteFilePath())
        return src.absoluteFilePath(); // already there
    QFile in(lutFile);
    if (!in.open(QIODevice::ReadOnly))
        return failWith(error, QObject::tr("Can't read %1.").arg(QDir::toNativeSeparators(lutFile)));
    return writeLut(dir, src.fileName(), in.readAll(), error);
}

QList<ZippedLut> lutsInZip(const QString &zipFile, int *preferred, QString *error)
{
    if (preferred)
        *preferred = -1;
    const auto archive = openZip(zipFile, error);
    if (!archive)
        return {};
    QList<ZippedLut> luts;
    for (const zip::Entry &e : archive->entries()) {
        const QString entry = QString::fromStdString(e.name);
        const QString name = entry.section(QLatin1Char('/'), -1);
        if (e.isDirectory() || entry.startsWith(QLatin1String("__MACOSX/")) ||
            name.startsWith(QLatin1Char('.')) ||
            !name.endsWith(QLatin1String(".cube"), Qt::CaseInsensitive))
            continue;
        luts.append({name, entry, qint64(e.size)});
    }
    if (luts.isEmpty()) {
        failWith(error, QObject::tr("%1 has no .cube LUT in it.").arg(QFileInfo(zipFile).fileName()));
        return {};
    }
    if (preferred) {
        auto rec709 = [](const ZippedLut &l) { return l.name.contains(QLatin1String("709")); };
        const bool any709 = std::any_of(luts.begin(), luts.end(), rec709);
        for (int i = 0; i < luts.size(); ++i) {
            if (any709 && !rec709(luts[i]))
                continue;
            if (*preferred < 0 || luts[i].size > luts[*preferred].size)
                *preferred = i;
        }
    }
    return luts;
}

QString importLutFromZip(const QString &projectFile, const QString &zipFile, const QString &entry,
                         QString *error)
{
    const auto archive = openZip(zipFile, error);
    if (!archive)
        return {};
    const auto &entries = archive->entries();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const zip::Entry &e) { return e.name == entry.toStdString(); });
    const QString name = entry.section(QLatin1Char('/'), -1);
    std::string text, why;
    if (it == entries.end() || name.isEmpty() || name.startsWith(QLatin1Char('.')))
        return failWith(error, QObject::tr("%1 isn't in %2.").arg(entry, QFileInfo(zipFile).fileName()));
    if (!archive->read(std::size_t(it - entries.begin()), &text, &why) ||
        !color::parseCube(text, &why))
        return failWith(error, QObject::tr("%1 can't be used as a LUT: %2.")
                                   .arg(name, QString::fromStdString(why)));
    QString dir = lutsDir(projectFile);
    if (dir.isEmpty())
        dir = unpackedLutsDir();
    return writeLut(dir, name, QByteArray::fromStdString(text), error);
}

QString unpackedLutsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/luts");
}

namespace {

// Where project `name` would live beside `projectFile`: its file, and the
// folder or file that must not exist yet.
struct Target {
    QString file;
    QString taken;
};

Target targetFor(const QString &projectFile, const QString &name)
{
    const QFileInfo fi(projectFile);
    if (isProjectFolder(projectFile)) {
        const QString parent = QFileInfo(fi.absolutePath()).absolutePath();
        return {projectFilePath(parent, name), QDir::cleanPath(parent + QLatin1Char('/') + name)};
    }
    const QString file = QDir::cleanPath(fi.absolutePath() + QLatin1Char('/') + name +
                                         QLatin1Char('.') +
                                         QLatin1String(Project::kFileExtension));
    return {file, file};
}

bool copyTree(const QString &from, const QString &to)
{
    QDir src(from);
    if (!src.exists())
        return true;
    if (!QDir().mkpath(to))
        return false;
    for (const QFileInfo &e :
         src.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString dest = to + QLatin1Char('/') + e.fileName();
        if (e.isDir() && !e.isSymLink()) {
            if (!copyTree(e.absoluteFilePath(), dest))
                return false;
        } else if (!QFile::copy(e.absoluteFilePath(), dest)) {
            return false;
        }
    }
    return true;
}

} // namespace

QString rename(const QString &projectFile, const QString &newName, QString *error)
{
    if (const QString problem = nameProblem(newName); !problem.isEmpty())
        return failWith(error, problem);
    const QFileInfo fi(projectFile);
    if (!fi.isFile())
        return failWith(error, QObject::tr("%1 doesn't exist.").arg(projectFile));
    if (newName == fi.completeBaseName())
        return fi.absoluteFilePath();
    const Target target = targetFor(projectFile, newName);
    // A name differing only in case is the same folder on a case-insensitive
    // file system; there the rename itself is still fine.
    if (QFileInfo::exists(target.taken) &&
        QFileInfo(target.taken).canonicalFilePath() !=
            (isProjectFolder(projectFile) ? QFileInfo(fi.absolutePath()).canonicalFilePath()
                                          : fi.canonicalFilePath()))
        return failWith(error, QObject::tr("%1 already exists. Choose another name.")
                                   .arg(QDir::toNativeSeparators(target.taken)));

    // An autosave recovery file goes along (Autosave::pathFor).
    const QString recovery = Autosave::pathFor(fi.absoluteFilePath());
    if (!isProjectFolder(projectFile)) {
        if (!QFile::rename(fi.absoluteFilePath(), target.file))
            return failWith(error, QObject::tr("Can't rename %1.")
                                       .arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
        if (QFileInfo::exists(recovery))
            QFile::rename(recovery, Autosave::pathFor(target.file));
        return target.file;
    }
    // The file inside first, then the folder; undone if the folder can't move.
    const QString folder = fi.absolutePath();
    const QString renamedFile = folder + QLatin1Char('/') + newName + QLatin1Char('.') +
                                QLatin1String(Project::kFileExtension);
    if (!QFile::rename(fi.absoluteFilePath(), renamedFile))
        return failWith(error, QObject::tr("Can't rename %1.")
                                   .arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
    if (!QDir().rename(folder, target.taken)) {
        QFile::rename(renamedFile, fi.absoluteFilePath());
        return failWith(error, QObject::tr("Can't rename the folder %1.")
                                   .arg(QDir::toNativeSeparators(folder)));
    }
    const QString movedRecovery =
        Autosave::pathFor(target.taken + QLatin1Char('/') + fi.fileName());
    if (QFileInfo::exists(movedRecovery))
        QFile::rename(movedRecovery, Autosave::pathFor(target.file));
    return target.file;
}

QString duplicate(const QString &projectFile, const QString &newName, QString *error)
{
    if (const QString problem = nameProblem(newName); !problem.isEmpty())
        return failWith(error, problem);
    const QFileInfo fi(projectFile);
    if (!fi.isFile())
        return failWith(error, QObject::tr("%1 doesn't exist.").arg(projectFile));
    const Target target = targetFor(projectFile, newName);
    if (QFileInfo::exists(target.taken))
        return failWith(error, QObject::tr("%1 already exists. Choose another name.")
                                   .arg(QDir::toNativeSeparators(target.taken)));

    if (!isProjectFolder(projectFile)) {
        QString err;
        if (!Project::copyFile(fi.absoluteFilePath(), target.file, &err))
            return failWith(error, err);
        return target.file;
    }
    const QString parent = QFileInfo(fi.absolutePath()).absolutePath();
    const QString file = create(parent, newName, error);
    if (file.isEmpty())
        return {};
    QString err;
    if (!Project::copyFile(fi.absoluteFilePath(), file, &err) ||
        !copyTree(cacheDir(projectFile), cacheDir(file)) ||
        !copyTree(lutsDir(projectFile), lutsDir(file))) {
        QDir(target.taken).removeRecursively();
        return failWith(error, err.isEmpty() ? QObject::tr("Can't copy the project's cache.") : err);
    }
    return file;
}

QString copyName(const QString &projectFile)
{
    const QString base = QFileInfo(projectFile).completeBaseName();
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? QObject::tr("%1 copy").arg(base)
                                    : QObject::tr("%1 copy %2").arg(base).arg(n);
        if (!QFileInfo::exists(targetFor(projectFile, name).taken))
            return name;
    }
}

QString ensure(const QString &dir)
{
    if (dir.isEmpty() || !QDir().mkpath(dir))
        return {};
    return dir;
}

} // namespace projectfolder
