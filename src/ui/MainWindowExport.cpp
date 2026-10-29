// MainWindow's export and screenshot: the flat-video export dialog, per-clip
// export jobs, source-presence checks, and still capture through the same
// path the export uses. Declarations stay in MainWindow.h.

#include "MainWindow.h"

#include <QFileInfo>
#include <QMessageBox>
#include <QMetaObject>

#include <array>
#include <cmath>

#include "EditToolbar.h"
#include "ExportOptionsDialog.h"
#include "ExportRunner.h"
#include "PlaybackController.h"
#include "PlayerBar.h"
#include "ReframeExportController.h"
#include "RenderJobs.h"
#include "SystemCheck.h"
#include "panels/ReframePanel.h"
#include "../core/ClipItem.h"
#include "../core/Project.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

void MainWindow::exportFlatVideo()
{
    if (m_exports->exporting())
        return;
    const int nclips = m_timeline->count();
    if (nclips == 0)
        return;
    // Checked first: without a hardware encoder the export would only fail
    // once FFmpeg opens the encoder, with its own error text.
    if (!syscheck::hwEncoderAvailable()) {
        QMessageBox box(QMessageBox::Warning, tr("Export flat video"),
                        syscheck::hwEncoderMissingText(), QMessageBox::Ok, this);
        // What each encoder lacks, for a bug report.
        box.setDetailedText(syscheck::hwEncoderReport());
        box.exec();
        return;
    }
    // Every source looked for on the disk now (the marks follow), not as last
    // checked: a missing main clip would only fail partway through, a
    // missing overlay or sound clip would be left out without a word. Either
    // way the user sorts it out first.
    recheckMissingMedia();
    if (!checkSourcesPresent(
            renderjobs::missingSourceLines(*m_timeline, renderjobs::MissingFor::Export,
                                            m_project->mediaPool())))
        return;

    // One output frame rate for the whole timeline: the project's (Project
    // Settings), or clip 0's when it's "match first clip". Each clip is
    // conformed to it -- frames dropped or repeated by source time, at the
    // clip's speed (ReframeExportController).
    const double fps = m_project->output().effectiveFps(m_timeline->clipAt(0).fps);

    // A clip with the privacy blur on is only exported once all of it that
    // shows has been scanned for plates: its trimmed range, and the footage
    // past it that a transition plays.
    for (int i = 0; i < nclips; ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        qint64 from = c.trimInMs, to = c.trimOutMs;
        m_timeline->shownSourceRange(c.id, from, to);
        if (const QString why = m_privacy.blocker(c, from, to); !why.isEmpty()) {
            QMessageBox::warning(this, tr("Export flat video"), why);
            return;
        }
    }
    {
        QStringList stale;
        for (int i = 0; i < nclips; ++i) {
            const ClipItem &c = m_timeline->clipAt(i);
            qint64 from = c.trimInMs, to = c.trimOutMs;
            m_timeline->shownSourceRange(c.id, from, to);
            if (const QString w = m_privacy.staleness(c, from, to, true); !w.isEmpty())
                stale << w;
        }
        if (!confirmStalePrivacy(tr("Export flat video"), stale))
            return;
    }

    qint64 totalRangeMs = 0;
    int totalKeys = 0;
    QStringList keyless; // "2 · name (at 00:12)", in timeline order
    for (int i = 0; i < nclips; ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        if (c.reframe.isEmpty()) {
            const qint64 s = totalRangeMs / 1000;
            keyless << tr("%1 · %2 (at %3:%4)")
                           .arg(i + 1)
                           .arg(QFileInfo(c.sourcePath).fileName().toHtmlEscaped())
                           .arg(s / 60, 2, 10, QLatin1Char('0'))
                           .arg(s % 60, 2, 10, QLatin1Char('0'));
        }
        totalRangeMs += c.timelineDurationMs();
        totalKeys += c.reframe.count();
    }
    if (totalRangeMs < 100) {
        QMessageBox::warning(this, tr("Export flat video"),
                             tr("The timeline is too short to export."));
        return;
    }

    ExportOptionsDialog opts(nclips, totalRangeMs, totalKeys, fps, keyless, m_project->output(),
                             this);
    if (opts.exec() != QDialog::Accepted)
        return;
    opts.rememberQuality();

    QString path = askSaveFileName(tr("Export flat video"),
                                   renderjobs::suggestedExportPath(m_project->filePath()),
                                   tr("MP4 video (*.mp4)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".mp4");

    ReframeExportController::Request req;
    req.outPath = path;
    req.fps = fps;
    req.outWidth = opts.outWidth();
    req.outHeight = opts.outHeight();
    req.h265 = opts.h265();
    req.tenBit = opts.tenBit();
    req.bitrateKbps = opts.bitrateKbps();

    // Per clip: the reframe track and the built stabilizer; the export samples
    // both per decoded frame at its own source time.
    for (int i = 0; i < nclips; ++i)
        req.clips.push_back(exportJobFor(m_timeline->clipAt(i)));
    req.mix = renderjobs::mixClips(*m_timeline); // the audio tracks
    // The overlay tracks, over the flat main clips.
    req.insets = renderjobs::insetJobs(*m_timeline, req.outWidth, req.outHeight, m_privacy,
                                       fieldSources(true));

    // The export opens its own NativeStitchSource (on the shared decode
    // device) -- pause playback so the two stitch pipelines don't contend
    // for the GPU.
    QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);

    m_exports->startExport(req);
}

ReframeExportController::ClipJob MainWindow::exportJobFor(const ClipItem &c) const
{
    return renderjobs::clipJob(c, *m_timeline, m_privacy);
}

bool MainWindow::checkSourcesPresent(const QStringList &missing)
{
    if (missing.isEmpty())
        return true;
    renderjobs::showMissingMedia(
        this, tr("Export flat video"),
        tr("The timeline can't be exported: these source files are missing."), missing,
        tr("If a file was moved or renamed, find it with Locate file… on it in the "
           "Media tab. Or remove its clips from the timeline."));
    return false;
}

void MainWindow::updateExportEnabled()
{
    m_exportAction->setEnabled(!m_exports->exporting() && m_timeline->count() > 0 && m_fps > 0.0);
}

void MainWindow::updateScreenshotEnabled()
{
    const bool on = !m_exports->savingStill() && !m_exports->exporting() &&
                    m_timeline->count() > 0 && m_fps > 0.0;
    m_screenshotAction->setEnabled(on);
    m_playerBar->setScreenshotEnabled(on);
    // While tracking, the button is the Stop control.
    m_reframePanel->setTrackEnabled(trackingRunning() ||
                                    (!m_exports->exporting() && m_timeline->count() > 0 && m_fps > 0.0));
}

void MainWindow::takeScreenshot()
{
    if (m_exports->savingStill() || m_exports->exporting())
        return;
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c)
        return;
    if (const QString why = m_privacy.blocker(*c, m_lastPositionMs, m_lastPositionMs); !why.isEmpty()) {
        QMessageBox::warning(this, tr("Screenshot"), why);
        return;
    }
    // In a transition, the other clip shows too: past its trim.
    for (const renderjobs::TransitionLayer &l : renderjobs::transitionLayers(*m_timeline)) {
        if (l.clipIndex < 0)
            continue; // black
        const ClipItem &o = m_timeline->clipAt(l.clipIndex);
        const qint64 end = l.startMs + std::llround((l.trimOutMs - l.trimInMs) / o.speed);
        if (m_lastTimelineMs < l.startMs || m_lastTimelineMs >= end)
            continue;
        const qint64 at = l.trimInMs + std::llround((m_lastTimelineMs - l.startMs) * o.speed);
        if (const QString why = m_privacy.blocker(o, at, at); !why.isEmpty()) {
            QMessageBox::warning(this, tr("Screenshot"), why);
            return;
        }
    }
    // A frame either side: the playhead's time is not the shown frame's.
    const qint64 frameMs = std::llround(1000.0 / c->fps);
    if (const QString w = m_privacy.staleness(*c, m_lastPositionMs - frameMs, m_lastPositionMs + frameMs, true);
        !confirmStalePrivacy(tr("Screenshot"), w.isEmpty() ? QStringList() : QStringList{w}))
        return;

    // Captured before the save dialog, so a playing timeline doesn't move the
    // frame on: the export's view of the clip, looked at through whatever the
    // viewport shows now -- the sampled reframe path, or a free look / drag.
    // A split clip: every pane as the viewport shows it, the active one's
    // view being the viewport's own.
    ExportRunner::Still still;
    still.job = exportJobFor(*c);
    std::array<view::Pose, pane::kMaxPanes> shown;
    m_viewport->PanePoses(shown.data());
    still.pose = shown[0];
    if (still.job.split.isSplit())
        still.job.panePoseAt = [shown](qint64, int pane, view::Pose &pose) {
            if (pane >= 0 && pane < pane::kMaxPanes)
                pose = shown[size_t(pane)];
        };
    still.sourceMs = m_lastPositionMs;
    still.timelineMs = std::max<qint64>(0, m_lastTimelineMs);
    still.width = m_project->output().width;
    still.height = m_project->output().height;

    QString path = askSaveFileName(
        tr("Save screenshot"),
        renderjobs::suggestedScreenshotPath(m_project->filePath(), c->sourcePath, still.timelineMs),
        tr("PNG image (*.png)"));
    if (path.isEmpty() || m_exports->savingStill() || m_exports->exporting())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".png");
    still.path = path;

    m_editToolbar->showNotice(tr("Saving screenshot…"), true);
    // The overlays showing at the playhead, composited like the export does.
    still.insets = renderjobs::insetJobs(*m_timeline, still.width, still.height, m_privacy,
                                         fieldSources(true));
    m_exports->startStill(std::move(still));
}
