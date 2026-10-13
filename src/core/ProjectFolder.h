#pragma once

#include <QString>

// A project's own folder: everything that belongs to one project in one place,
// so deleting the folder removes it all. Media is referenced, not copied.
//
//   <name>/<name>.orp   the project file
//   <name>/cache/       plate scans, waveforms, thumbnails (rebuilt if deleted)
//   <name>/exports/     where exports are suggested
//   <name>/screenshots/ where screenshots are suggested
//
// A project file that isn't <name>/<name>.orp is a loose one (projects saved
// before project folders existed): it still opens and saves where it is, and
// uses the shared user cache.
namespace projectfolder {

// The project file of project `name` created under `parentDir`.
QString projectFilePath(const QString &parentDir, const QString &name);

// Why `name` can't name a project (it becomes a folder and a file name), or
// empty when it can.
QString nameProblem(const QString &name);

// Make <parentDir>/<name>/ with its sub-folders. Fails, with *error set, on a
// bad name or when <parentDir>/<name> already exists. Returns the path of the
// project file to save (not written here).
QString create(const QString &parentDir, const QString &name, QString *error = nullptr);

// True when `projectFile` is <name>/<name>.orp, inside its own folder.
bool isProjectFolder(const QString &projectFile);

// The folders of the project whose file is `projectFile`; empty for a loose
// project file (or none). Not created here (see ensure()).
QString cacheDir(const QString &projectFile);
QString exportsDir(const QString &projectFile);
QString screenshotsDir(const QString &projectFile);

// `dir` made if missing (a folder deleted by hand); `dir` itself on success,
// empty when it can't be made or `dir` is empty.
QString ensure(const QString &dir);

} // namespace projectfolder
