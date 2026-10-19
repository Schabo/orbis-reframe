#include "MediaImporter.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QStorageInfo>
#include <QThreadPool>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>

#include "MediaPoolPanel.h"
#include "TabSidebar.h"
#include "../core/Project.h"

namespace {

QString addingFilesText(int pending)
{
    return pending == 1 ? MediaImporter::tr("Adding 1 file…")
                        : MediaImporter::tr("Adding %1 files…").arg(pending);
}

// Removable volumes a camera's files sit on: anything mounted under the
// usual media roots.
QStringList deviceRoots()
{
    QStringList roots;
    for (const QStorageInfo &v : QStorageInfo::mountedVolumes()) {
        if (!v.isValid() || !v.isReady() || v.isRoot())
            continue;
        const QString p = QFileInfo(v.rootPath()).absoluteFilePath();
        if (p.startsWith(QLatin1String("/run/media/")) ||
            p.startsWith(QLatin1String("/media/")) || p.startsWith(QLatin1String("/mnt/")))
            roots << p;
    }
    roots.removeDuplicates();
    return roots;
}

bool underAnyRoot(const QStringList &roots, const QString &path)
{
    return std::any_of(roots.begin(), roots.end(), [&](const QString &r) {
        return path.startsWith(r + QLatin1Char('/'));
    });
}

// `name` somewhere below a device root -- DCIM first (cards hold their files
// there), the rest of the tree after.
QString findOnDevice(const QStringList &roots, const QString &name)
{
    for (const bool dcimFirst : {true, false}) {
        for (const QString &root : roots) {
            const QString base = dcimFirst && QDir(root + QLatin1String("/DCIM")).exists()
                                     ? root + QLatin1String("/DCIM")
                                     : root;
            QDirIterator it(base, {name}, QDir::Files | QDir::Readable,
                            QDirIterator::Subdirectories);
            if (it.hasNext())
                return QFileInfo(it.next()).absoluteFilePath();
        }
    }
    return {};
}

// A name free under `dir`: the file's own, or name-2, name-3, …
QString freeName(const QDir &dir, const QString &name)
{
    const QFileInfo fi(name);
    const QString stem = fi.completeBaseName(), suffix = fi.suffix();
    auto pathFor = [&](int n) {
        const QString candidate =
            n == 1 ? name
                   : stem + QStringLiteral("-%2.%3").arg(n).arg(suffix);
        return dir.absoluteFilePath(candidate);
    };
    QString path = pathFor(1);
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = pathFor(n);
    return path;
}

// `from` -> `to` in chunks, `done` holding the bytes copied across every
// job so far and `tick` called with it each chunk. False on a failed read
// or write, or when `cancel` goes true; a partial file is removed.
bool copyChunked(const QString &from, const QString &to, qint64 &done,
                 const std::atomic<bool> &cancel, const std::function<void(qint64)> &tick)
{
    QFile in(from), out(to);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly))
        return false;
    char buf[4 << 20];
    bool ok = true;
    while (!cancel.load(std::memory_order_relaxed)) {
        const qint64 n = in.read(buf, sizeof(buf));
        if (n < 0 || (n > 0 && out.write(buf, n) != n)) {
            ok = false;
            break;
        }
        done += n;
        tick(done);
        if (n == 0)
            break; // done
    }
    in.close();
    out.close();
    if (!ok || cancel.load(std::memory_order_relaxed)) {
        QFile::remove(to);
        return false;
    }
    return true;
}

} // namespace

MediaImporter::MediaImporter(Project *project, MediaPoolPanel *panel, TabSidebar *sidebar,
                             ClipProbeFn probe, QWidget *window)
    : QObject(window), m_project(project), m_panel(panel), m_sidebar(sidebar),
      m_probe(std::move(probe)), m_window(window)
{
}

void MediaImporter::importFiles()
{
    QStringList patterns;
    for (const auto kind :
         {MediaItem::Kind::Clip360, MediaItem::Kind::Video, MediaItem::Kind::Audio})
        for (const QString &ext : media::extensions(kind))
            patterns << QStringLiteral("*.") + ext;
    const QStringList paths = QFileDialog::getOpenFileNames(
        m_window, tr("Import files"), m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir,
        tr("Video and audio files (%1);;All files (*)").arg(patterns.join(QLatin1Char(' '))));
    if (paths.isEmpty())
        return;
    m_lastDir = QFileInfo(paths.first()).absolutePath();
    import(paths);
}

void MediaImporter::importFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        m_window, tr("Import folder"), m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir);
    if (dir.isEmpty())
        return;
    m_lastDir = dir;
    import({dir});
}

void MediaImporter::import(const QStringList &paths)
{
    // Folders: the files in them. Files already in the pool are left as
    // they are.
    QStringList files;
    QStringList emptyFolders;
    for (const QString &path : paths) {
        const QFileInfo fi(path);
        const QStringList found = fi.isDir() ? media::filesInFolder(path) : QStringList{path};
        if (fi.isDir() && found.isEmpty())
            emptyFolders << QDir::toNativeSeparators(fi.absoluteFilePath());
        for (const QString &f : found) {
            const QString abs = QFileInfo(f).absoluteFilePath();
            if (!m_project->mediaPool()->contains(abs) && !files.contains(abs))
                files << abs;
        }
    }
    if (!emptyFolders.isEmpty())
        QMessageBox::information(m_window, tr("Import folder"),
                                 tr("No video or audio files in:\n%1")
                                     .arg(emptyFolders.join(QLatin1Char('\n'))));
    if (files.isEmpty()) {
        // Already there: show them.
        QStringList present;
        for (const QString &path : paths)
            if (m_project->mediaPool()->contains(QFileInfo(path).absoluteFilePath()))
                present << QFileInfo(path).absoluteFilePath();
        m_panel->selectPaths(present);
        return;
    }

    // Probing reads every file's header (an .insv's trailer too): on worker
    // threads, so a card full of clips doesn't stall the window. Results land
    // one by one; those for a project that has been closed meanwhile are
    // dropped.
    m_sidebar->setOpen(true);
    m_pending += int(files.size());
    m_panel->setStatus(addingFilesText(m_pending));
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(2);
        return p;
    }();
    QPointer<MediaImporter> self(this);
    const int session = m_session;
    for (const QString &file : files) {
        pool->start([self, session, file, probe = m_probe] {
            QString error;
            const std::optional<MediaItem> item = probeMedia(file, &error, probe);
            QMetaObject::invokeMethod(
                qApp,
                [self, session, file, item, error] {
                    if (!self)
                        return;
                    if (session == self->m_session) { // still the same project
                        if (item)
                            self->m_project->mediaPool()->add(*item);
                        else
                            self->m_failures
                                << QFileInfo(file).fileName() + QStringLiteral(": ") + error;
                    }
                    if (--self->m_pending > 0) {
                        self->m_panel->setStatus(addingFilesText(self->m_pending));
                        return;
                    }
                    self->m_panel->setStatus(QString());
                    if (!self->m_failures.isEmpty()) {
                        const QString failed = self->m_failures.join(QLatin1Char('\n'));
                        self->m_failures.clear();
                        QMessageBox::warning(self->m_window, tr("Could not import"),
                                             tr("These files can't be read:\n%1").arg(failed));
                    }
                },
                Qt::QueuedConnection);
        });
    }
}

void MediaImporter::collectIntoProject()
{
    if (m_collecting)
        return;
    if (m_project->filePath().isEmpty()) {
        QMessageBox::information(
            m_window, tr("Copy media into project"),
            tr("Save the project first -- the copies land in a Media folder next to it."));
        return;
    }
    const QDir projectDir = QFileInfo(m_project->filePath()).absoluteDir();
    auto inside = [&](const QString &p) {
        const QString rel = projectDir.relativeFilePath(p);
        return !rel.startsWith(QLatin1String("../")) && !QDir::isAbsolutePath(rel);
    };
    const QStringList roots = deviceRoots();

    // Where each pool file outside the project folder can be copied from:
    // its own path when that's on a device, else its name found on one.
    // Files that exist locally away from a device are left alone.
    struct Job {
        QString poolPath;
        QString source;
    };
    QList<Job> jobs;
    auto unresolved = [&] {
        QList<MediaItem> out;
        for (const MediaItem &m : m_project->mediaPool()->items()) {
            const QString abs = QFileInfo(m.path).absoluteFilePath();
            if (inside(abs))
                continue; // already in the project folder
            if (QFileInfo::exists(abs) && !underAnyRoot(roots, abs))
                continue; // present and local -- nothing to gather
            if (std::none_of(jobs.begin(), jobs.end(),
                             [&](const Job &j) { return j.poolPath == m.path; }))
                out << m;
        }
        return out;
    };
    auto resolve = [&](const MediaItem &m) -> QString {
        const QString abs = QFileInfo(m.path).absoluteFilePath();
        if (QFileInfo::exists(abs))
            return underAnyRoot(roots, abs) ? abs : QString();
        return findOnDevice(roots, QFileInfo(abs).fileName());
    };
    for (const MediaItem &m : unresolved())
        if (const QString src = resolve(m); !src.isEmpty())
            jobs.append({m.path, src});

    // What the mounts didn't have: a folder the user points at once (the
    // fallback when no device was detected at all).
    const QList<MediaItem> left = unresolved();
    if (!left.isEmpty()) {
        const QString picked =
            QFileDialog::getExistingDirectory(m_window, tr("Look for media files in"));
        if (!picked.isEmpty())
            for (const MediaItem &m : left)
                if (const QString src =
                        findOnDevice({picked}, QFileInfo(m.path).fileName());
                    !src.isEmpty())
                    jobs.append({m.path, src});
    }
    if (jobs.isEmpty()) {
        QMessageBox::information(m_window, tr("Copy media into project"),
                                 tr("No media files to copy in -- everything the project uses "
                                    "is already in its folder or wasn't found."));
        return;
    }
    QStringList notFound;
    for (const MediaItem &m : unresolved())
        notFound << QFileInfo(m.path).fileName();

    const QString mediaDirPath = projectDir.absoluteFilePath(QStringLiteral("Media"));
    QDir().mkpath(mediaDirPath);
    const QDir mediaDir(mediaDirPath);

    // Destinations: an existing same-size file is the same copy (relink to
    // it, don't copy it again); a different one yields name-2.
    struct Plan {
        QString poolPath;
        QString source;
        QString dest;
        bool copy;
    };
    QList<Plan> plans;
    qint64 totalBytes = 0;
    for (const Job &j : jobs) {
        QString dest = mediaDir.absoluteFilePath(QFileInfo(j.source).fileName());
        bool copy = true;
        if (QFileInfo::exists(dest)) {
            if (QFileInfo(dest).size() == QFileInfo(j.source).size())
                copy = false;
            else
                dest = freeName(mediaDir, QFileInfo(j.source).fileName());
        }
        if (copy)
            totalBytes += QFileInfo(j.source).size();
        plans.append({j.poolPath, j.source, dest, copy});
    }

    const QStorageInfo space(mediaDirPath);
    if (space.isValid() && space.bytesAvailable() >= 0 && totalBytes > space.bytesAvailable()) {
        QMessageBox::warning(
            m_window, tr("Copy media into project"),
            tr("Not enough room in %1 -- the files need about %2, %3 free.")
                .arg(mediaDirPath)
                .arg(QLocale().formattedDataSize(totalBytes))
                .arg(QLocale().formattedDataSize(space.bytesAvailable())));
        return;
    }

    // Copy and probe on a worker; progress and the result come back queued.
    m_collecting = true;
    m_panel->setStatus(tr("Copying media into the project…"));
    auto *progress = new QProgressDialog(m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setWindowTitle(tr("Copy media into project"));
    progress->setMinimumDuration(0);
    progress->setRange(0, 1000);
    progress->setValue(0);
    progress->setLabelText(tr("Copying…"));
    progress->setModal(false);
    progress->show();

    QPointer<QProgressDialog> dlg(progress);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(progress, &QProgressDialog::canceled, this,
            [cancel] { cancel->store(true); });

    auto tick = [dlg](qint64 done, qint64 total) {
        QMetaObject::invokeMethod(
            qApp,
            [dlg, done, total] {
                if (dlg && total > 0)
                    dlg->setValue(int(done * 1000 / total));
            },
            Qt::QueuedConnection);
    };

    QPointer<MediaImporter> self(this);
    const int session = m_session;
    QThreadPool::globalInstance()->start(
        [self, session, dlg, plans, notFound, cancel, tick, probe = m_probe] {
            Relinks relinks;
            QStringList failures;
            qint64 done = 0, total = 0;
            for (const Plan &p : plans)
                if (p.copy)
                    total += QFileInfo(p.source).size();
            for (int i = 0; i < plans.size(); ++i) {
                const Plan &p = plans[i];
                if (cancel->load(std::memory_order_relaxed))
                    break;
                const QString name = QFileInfo(p.source).fileName();
                if (p.copy) {
                    QMetaObject::invokeMethod(
                        qApp,
                        [dlg, name, i, n = plans.size()] {
                            if (dlg)
                                dlg->setLabelText(QObject::tr("Copying %1 (%2 of %3)")
                                                      .arg(name)
                                                      .arg(i + 1)
                                                      .arg(n));
                        },
                        Qt::QueuedConnection);
                    if (!copyChunked(p.source, p.dest, done, *cancel,
                                     [&](qint64 d) { tick(d, total); })) {
                        if (!cancel->load(std::memory_order_relaxed))
                            failures << name + QStringLiteral(": ") +
                                            QObject::tr("couldn't be copied");
                        break; // cancel or io error stops the rest
                    }
                }
                tick(done, total);
                QString error;
                if (std::optional<MediaItem> item = probeMedia(p.dest, &error, probe))
                    relinks.append({p.poolPath, *item});
                else
                    failures << QFileInfo(p.dest).fileName() + QStringLiteral(": ") + error;
            }
            const bool cancelled = cancel->load(std::memory_order_relaxed);
            QMetaObject::invokeMethod(
                qApp,
                [self, session, dlg, relinks, failures, notFound, cancelled] {
                    if (dlg)
                        dlg->close();
                    if (!self)
                        return;
                    self->m_collecting = false;
                    self->m_panel->setStatus(QString());
                    if (cancelled)
                        return;
                    if (session == self->m_session && !relinks.isEmpty())
                        emit self->relinksReady(relinks);
                    QStringList notes;
                    if (!notFound.isEmpty())
                        notes << self->tr("Not found on the device:\n%1")
                                     .arg(notFound.join(QLatin1Char('\n')));
                    if (!failures.isEmpty())
                        notes << self->tr("Couldn't copy:\n%1")
                                     .arg(failures.join(QLatin1Char('\n')));
                    if (!notes.isEmpty())
                        QMessageBox::warning(self->m_window, self->tr("Copy media into project"),
                                             notes.join(QStringLiteral("\n\n")));
                },
                Qt::QueuedConnection);
        });
}

std::optional<MediaItem> MediaImporter::probeSame(const MediaItem &was, const QString &file,
                                                  QString *why) const
{
    // The same file, moved or renamed: same kind, same length (and size for
    // video). Anything else would leave the clips' trims and keys pointing
    // at the wrong frames.
    QString error;
    std::optional<MediaItem> m = probeMedia(file, &error, m_probe);
    if (!m) {
        *why = tr("It can't be read: %1").arg(error);
        return m;
    }
    const auto kindName = [](MediaItem::Kind k) {
        return k == MediaItem::Kind::Clip360 ? tr("a 360° clip")
               : k == MediaItem::Kind::Video ? tr("a video")
                                             : tr("an audio file");
    };
    const auto length = [](qint64 ms) {
        const qint64 s = (ms + 500) / 1000;
        return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
    };
    if (m->kind != was.kind)
        *why = tr("It isn't the same file: it is %1, the missing one %2.").arg(kindName(m->kind), kindName(was.kind));
    else if (was.durationMs > 0 && std::abs(m->durationMs - was.durationMs) > 1000)
        *why = tr("It isn't the same file: it is %1 long, the missing one %2.")
                   .arg(length(m->durationMs), length(was.durationMs));
    else if (was.width > 0 && m->width > 0 &&
             (was.width != m->width || was.height != m->height))
        *why = tr("It isn't the same file: it is %1×%2, the missing one %3×%4.")
                   .arg(m->width).arg(m->height).arg(was.width).arg(was.height);
    else
        return m;
    m.reset();
    return m;
}

MediaImporter::Relinks MediaImporter::locate(const QString &path)
{
    const MediaItem *missing = m_project->mediaPool()->item(path);
    if (!missing || !missing->missing)
        return {};
    const MediaItem old = *missing;
    const QFileInfo oldInfo(path);

    // Start where it was, or as close to it as still exists.
    QDir start(oldInfo.absolutePath());
    while (!start.exists() && start.cdUp()) {
    }
    QStringList patterns;
    for (const QString &ext : media::extensions(old.kind))
        patterns << QStringLiteral("*.") + ext;
    const QString picked = QFileDialog::getOpenFileName(
        m_window, tr("Locate %1").arg(oldInfo.fileName()),
        start.exists() ? start.absolutePath() : QDir::homePath(),
        tr("%1 (%2);;All files (*)")
            .arg(oldInfo.fileName(), patterns.join(QLatin1Char(' '))));
    if (picked.isEmpty())
        return {};

    const QString target = QFileInfo(picked).absoluteFilePath();
    QString why;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::optional<MediaItem> found = probeSame(old, target, &why);
    QApplication::restoreOverrideCursor();
    if (!found) {
        QMessageBox::warning(m_window, tr("Locate file"),
                             tr("%1 can't replace %2.\n%3")
                                 .arg(QFileInfo(target).fileName(), oldInfo.fileName(), why));
        return {};
    }

    Relinks relinks{{path, *found}};
    // Other missing files with their names in the same folder: a card or
    // folder that moved as a whole.
    const QDir newDir = QFileInfo(target).absoluteDir();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const MediaItem &m : m_project->mediaPool()->items()) {
        if (!m.missing || m.path == path)
            continue;
        const QString candidate = newDir.absoluteFilePath(QFileInfo(m.path).fileName());
        if (!QFileInfo::exists(candidate) || m_project->mediaPool()->contains(candidate))
            continue;
        QString ignored;
        if (std::optional<MediaItem> same = probeSame(m, candidate, &ignored))
            relinks.append({m.path, *same});
    }
    QApplication::restoreOverrideCursor();
    return relinks;
}

void MediaImporter::showRelinked(const Relinks &relinks)
{
    if (relinks.isEmpty())
        return;
    m_panel->selectPaths({relinks.first().second.path});
    if (relinks.size() > 1) {
        QStringList names;
        for (qsizetype i = 1; i < relinks.size(); ++i)
            names << QFileInfo(relinks[i].second.path).fileName();
        QMessageBox::information(m_window, tr("Locate file"),
                                 tr("Other missing files were in the same folder too, and are "
                                    "found again:\n%1")
                                     .arg(names.join(QLatin1Char('\n'))));
    }
}
