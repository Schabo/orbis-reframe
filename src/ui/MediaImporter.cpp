#include "MediaImporter.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QThreadPool>

#include <cstdlib>

#include "MediaPoolPanel.h"
#include "TabSidebar.h"
#include "../core/Project.h"

namespace {

QString addingFilesText(int pending)
{
    return pending == 1 ? MediaImporter::tr("Adding 1 file…")
                        : MediaImporter::tr("Adding %1 files…").arg(pending);
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
