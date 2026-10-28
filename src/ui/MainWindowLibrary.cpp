// MainWindow's project library and open/close flow: the start screen, new
// and open project, binding an empty timeline, and the window title / cache
// dirs that follow the current project file. Declarations stay in MainWindow.h.

#include "MainWindow.h"

#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPlatformSurfaceEvent>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>

#include "EditToolbar.h"
#include "LensGuardCache.h"
#include "MediaImporter.h"
#include "ProjectFiles.h"
#include "ProjectLibraryPage.h"
#include "RenderJobs.h"
#include "TimelineWidget.h"
#include "ThumbnailCache.h"
#include "WaveformCache.h"
#include "panels/AudioPanel.h"
#include "panels/ColourPanel.h"
#include "panels/MediaInfoPanel.h"
#include "panels/PrivacyPanel.h"
#include "panels/ReframePanel.h"
#include "panels/StabilizationPanel.h"
#include "panels/StitchingPanel.h"
#include "../core/Autosave.h"
#include "../core/ClipItem.h"
#include "../core/Project.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

void MainWindow::openPath(const QString &path)
{
    if (QFileInfo(path).suffix().compare(QLatin1String(Project::kFileExtension),
                                         Qt::CaseInsensitive) == 0)
        openProject(path);
    else
        openClip(path);
}

void MainWindow::openClip(const QString &clipPath)
{
    stitch::ClipProbe probe;
    std::string probeErr;
    if (!m_clipBackend.probe(clipPath.toStdString(), probe, probeErr)) {
        QMessageBox::warning(this, tr("Could not open clip"),
                             probeErr.empty() ? tr("Unrecognised file: %1").arg(clipPath)
                                              : QString::fromStdString(probeErr));
        return;
    }
    m_clipPath = clipPath;
    m_fps = probe.fps;
    m_rebindingModel = true;
    m_lensGuards->set(clipPath, probe.lens_guards);
    m_clipId = m_project->resetWithClip(clipPath, probe.duration_ms, probe.fps, probe.width,
                                        probe.height, audioStreamCount(clipPath), probe.lens_guards);
    m_rebindingModel = false;
    if (const ClipItem *c = m_timeline->clipById(m_clipId))
        applyColorGrade(c->color);
    refreshClipPanels();
    showEditor();
    syncTimeline(); // opens paused on the first frame
}

void MainWindow::bindEmptyTimeline()
{
    m_clipId = 0;
    m_clipPath.clear();
    m_fps = 0.0;
    m_audioPanel->setNoClip();
    m_colourPanel->setNoClip();
    m_stabilizationPanel->setNoClip();
    m_stitchingPanel->setNoClip();
    m_privacyPanel->setNoClip();
    m_mediaInfoPanel->setNoClip();
    m_reframePanel->setNoClip();
    // Nothing of the last project carries over: the view looks straight
    // ahead again, from the first pane, and reframe authoring starts
    // disarmed.
    setActivePane(0);
    m_viewport->SetPose(ReframePose{});
    m_reframePanel->setClipUnkeyed(false);
    m_editToolbar->setReframeArmed(false);
    m_editToolbar->setHasReframeKeys(false);
    syncTimeline(); // empty -> controller goes idle, widget clears
    syncReframePanel();
    updateSeamStatus();
    updateSyncStatus();
}

void MainWindow::openProject(const QString &projectPath)
{
    QString error;
    const bool recover = m_files->askRecover(projectPath);
    const QString recovery = Autosave::pathFor(projectPath);
    // Before the load: rebuilding the timeline already asks for waveforms.
    updateCacheDirs(projectPath);
    m_rebindingModel = true;
    const bool loaded = recover ? m_project->recover(recovery, projectPath, &error)
                                : m_project->load(projectPath, &error);
    if (loaded && recover)
        m_autosave->tick(); // the recovery file is this session's now
    m_rebindingModel = false;
    if (!loaded) {
        updateCacheDirs(m_project->filePath());
        QMessageBox::warning(this, tr("Could not open project"), error);
        return;
    }
    m_files->noteOpened();
    showEditor();
    if (m_timeline->count() == 0) {
        bindEmptyTimeline();
        return;
    }

    const ClipItem &first = m_timeline->clipAt(0);
    m_clipId = first.id;
    m_clipPath = first.sourcePath;
    m_fps = first.fps;
    applyColorGrade(first.color);
    refreshClipPanels(); // the first clip's: a load clears the selection

    // All of them, labelled with their place on the timeline (hidden and
    // muted tracks too: unhiding one later doesn't warn again). The model
    // stays on screen; playback shows a missing clip black.
    if (const QStringList missing =
            renderjobs::missingSourceLines(*m_timeline, renderjobs::MissingFor::Timeline,
                                            m_project->mediaPool());
        !missing.isEmpty())
        renderjobs::showMissingMedia(
            this, tr("Missing media"),
            tr("Source files are not where the project expects them:"), missing,
            tr("If a file was moved or renamed, find it with Locate file… on it in "
               "the Media tab."));
    syncTimeline(); // opens paused on the first frame
}

void MainWindow::openProjectFile()
{
    if (!m_files->confirmDiscard())
        return;
    if (const QString path = m_files->pickProjectFile(); !path.isEmpty())
        openProject(path);
}

void MainWindow::newProject()
{
    if (!m_files->confirmDiscard())
        return;
    const std::optional<ProjectFiles::NewProject> made = m_files->askNewProject();
    if (!made)
        return;
    m_rebindingModel = true;
    m_project->newProject();
    m_project->setOutput(made->output);
    m_rebindingModel = false;
    // Saved straight away, so the project is on disk and in the library
    // from the start.
    QString error;
    if (!m_project->save(made->file, &error))
        QMessageBox::warning(this, tr("Could not save project"), error);
    else
        m_files->noteOpened();
    showEditor();
    bindEmptyTimeline();
}

void MainWindow::closeToLibrary()
{
    if (!m_files->confirmDiscard())
        return;
    stopPlateJob(); // its scan is about clips that are going away
    m_rebindingModel = true;
    m_project->newProject();
    m_rebindingModel = false;
    bindEmptyTimeline();
    showLibrary();
}

void MainWindow::showLibrary()
{
    m_mediaImport->newSession();
    setEditorActive(false);
    if (m_pages->currentWidget() != m_libraryPage) {
        // Let the viewport release its swapchain while it's still shown (as
        // closeEvent does): hiding it first crashes some NVIDIA drivers. It
        // starts again on its own when the editor is shown.
        QPlatformSurfaceEvent aboutToGo(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
        QCoreApplication::sendEvent(m_viewport, &aboutToGo);
        m_pages->setCurrentWidget(m_libraryPage);
    }
    m_libraryPage->refresh();
    m_libraryPage->setFocus();
    updateWindowTitle();
}

void MainWindow::showEditor()
{
    m_mediaImport->newSession();
    m_pages->setCurrentWidget(m_editorPage);
    setEditorActive(true);
    updateWindowTitle();
    // Once the page has its height (the window may still be laying out).
    QTimer::singleShot(0, this, &MainWindow::fitTimelineHeight);
}

void MainWindow::setEditorActive(bool active)
{
    if (!active)
        setFullScreenView(false);
    for (QAction *a : std::as_const(m_editorActions))
        a->setEnabled(active);
    for (QShortcut *sc : std::as_const(m_editorShortcuts))
        sc->setEnabled(active);
}

void MainWindow::updateCacheDirs(const QString &projectFile)
{
    // A project folder's own cache/; the user cache for an untitled or loose
    // project (and if the folder can't be made).
    const QString dir = projectfolder::ensure(projectfolder::cacheDir(projectFile));
    m_privacy.scans().setDirectory(dir);
    m_waveforms->setDirectory(dir);
    if (m_thumbnailCache)
        m_thumbnailCache->setDirectory(dir);
}

QString MainWindow::askSaveFileName(const QString &caption, const QString &path,
                                    const QString &filter)
{
    QFileDialog dlg(this, caption, path, filter);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setLabelText(QFileDialog::Accept, tr("&Save"));
    if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty())
        return {};
    return dlg.selectedFiles().constFirst();
}

void MainWindow::updateWindowTitle()
{
    if (m_pages && m_pages->currentWidget() == m_libraryPage) {
        setWindowTitle(QStringLiteral("orbis-reframe"));
        return;
    }
    QString name = m_project && !m_project->filePath().isEmpty()
                       ? QFileInfo(m_project->filePath()).fileName()
                       : tr("untitled");
    const QString star =
        (m_project && m_project->modified()) ? QStringLiteral(" *") : QString();
    setWindowTitle(QStringLiteral("orbis-reframe — %1%2").arg(name, star));
}
