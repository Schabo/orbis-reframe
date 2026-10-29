// MainWindow's Stitching and Stabilization tabs: panel wiring, per-clip
// stabilizer cache publish, seam / sync status, and the View menu's preview
// stitch quality/size checkmarks. Declarations stay in MainWindow.h.

#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QMetaObject>

#include <cmath>

#include <IStitchSource.h> // stitch::PreviewStitchConfig

#include "ClipMeasurements.h"
#include "EditToolbar.h"
#include "LensGuardCache.h"
#include "PlaybackController.h"
#include "PlayerBar.h"
#include "TimelineWidget.h"
#include "panels/StabilizationPanel.h"
#include "panels/StitchingPanel.h"
#include "../core/ClipItem.h"
#include "../core/Project.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

void MainWindow::connectStitchAndStabilization()
{
    // Stitching settings: a panel edit goes onto the file of the clip it
    // shows (every clip cut from it), "Apply to all clips" onto every 360
    // clip's; clipStitchChanged below re-opens the stitch with them.
    connect(m_stitchingPanel, &StitchingPanel::stitchEdited, this,
            [this](StitchingPanel::LensGuard guard, bool depthSeam) {
                const ClipItem *c = m_timeline->clipById(editClipId());
                if (c && !c->flat)
                    m_timeline->setSourceStitch(c->sourcePath, guard, depthSeam);
            });
    connect(m_stitchingPanel, &StitchingPanel::applyToAllRequested, this, [this] {
        const ClipItem *c = m_timeline->clipById(editClipId());
        if (!c || c->flat)
            return;
        // The guard goes only where it means something: from a file with
        // guard profiles onto a file that has this one. A ONE X2 clip's
        // None doesn't strip an X5 clip's guards, and an X5 clip's Premium
        // doesn't land on a file that would stitch bare anyway.
        const bool depthSeam = c->depthSeam;
        const stitch::LensGuardProfiles from = m_lensGuards->profiles(c->sourcePath);
        const StitchingPanel::LensGuard guard = from.usable(c->lensGuard);
        for (int i = 0; i < m_timeline->count(); ++i) {
            const ClipItem &o = m_timeline->clipAt(i);
            if (o.flat)
                continue;
            const stitch::LensGuardProfiles to = m_lensGuards->profiles(o.sourcePath);
            const bool takesGuard = from.any() && to.any() && to.has(guard);
            m_timeline->setSourceStitch(o.sourcePath, takesGuard ? guard : o.lensGuard,
                                        depthSeam);
        }
    });
    // Project settings (File ▸ Project settings…): output aspect / size /
    // fps. The viewport is pinned to the output aspect; export reads the size
    // and rate from the Project when it starts.
    connect(m_project, &Project::outputChanged, this, [this](const OutputSettings &out) {
        m_viewport->SetOutputAspect(static_cast<float>(out.aspectRatio()));
        m_viewport->SetOutputSize(out.width, out.height); // where the panes go
        pushInsetsToController(); // flat main clips are fitted to the output's shape
    });
    m_viewport->SetOutputAspect(static_cast<float>(m_project->output().aspectRatio()));
    m_viewport->SetOutputSize(m_project->output().width, m_project->output().height);
    // Stabilization panel -> the clip it shows (the selected one, else the
    // one under the playhead), like the Colour panel. All modes drive the
    // native reprojection-time path; the panel hands back the right
    // insv::StabConfig for the mode (LeanFollow window from its Strength
    // slider, Custom from the knobs, Off otherwise).
    connect(m_stabilizationPanel, &StabilizationPanel::modeChanged, this,
            [this](StabilizationPanel::Mode, const insv::StabConfig &nativeCfg) {
                const ClipItem *c = m_timeline->clipById(editClipId());
                if (!c)
                    return;
                insv::StabConfig cfg = nativeCfg;
                // Switching into Direction lock, or ticking Custom's Lock
                // direction, holds the way the camera faces under the
                // playhead.
                if (cfg.lockDirection && !c->stabilization.lockDirection)
                    cfg.directionSec = directionSecAtPlayhead(*c);
                m_timeline->setClipStabilization(c->id, cfg);
            });
    connect(m_stabilizationPanel, &StabilizationPanel::lockDirectionRequested, this, [this] {
        const ClipItem *c = m_timeline->clipById(editClipId());
        if (!c || !c->stabilization.lockDirection)
            return;
        insv::StabConfig cfg = c->stabilization;
        cfg.directionSec = directionSecAtPlayhead(*c);
        m_timeline->setClipStabilization(c->id, cfg);
    });
    // Copy / paste a clip's stabilization, as the colour grade.
    const auto copyStab = [this](int clipId) {
        const ClipItem *c = m_timeline->clipById(clipId);
        if (!c)
            return;
        m_copiedStab = c->stabilization;
        m_copiedStabSource = c->sourcePath;
        m_stabilizationPanel->setCanPaste(true);
        m_timelineWidget->setCanPasteStabilization(true);
    };
    const auto pasteStab = [this](int clipId) {
        const ClipItem *c = m_timeline->clipById(clipId);
        if (m_copiedStab && c)
            m_timeline->setClipStabilization(
                clipId, pastedStabilization(*m_copiedStab, m_copiedStabSource, *c));
    };
    connect(m_stabilizationPanel, &StabilizationPanel::copyRequested, this,
            [this, copyStab] { copyStab(editClipId()); });
    connect(m_stabilizationPanel, &StabilizationPanel::pasteRequested, this,
            [this, pasteStab] { pasteStab(editClipId()); });
    connect(m_timelineWidget, &TimelineWidget::stabilizationCopyRequested, this, copyStab);
    connect(m_timelineWidget, &TimelineWidget::stabilizationPasteRequested, this, pasteStab);
    connect(m_timelineWidget, &TimelineWidget::stabilizationPasteAllRequested, this, [this] {
        if (!m_copiedStab)
            return;
        for (int i = 0; i < m_timeline->count(); ++i) {
            const ClipItem &c = m_timeline->clipAt(i);
            m_timeline->setClipStabilization(
                c.id, pastedStabilization(*m_copiedStab, m_copiedStabSource, c));
        }
    });
    // The preview's quality and size (View menu): the controller re-opens
    // its sources with them.
    connect(m_project, &Project::previewStitchChanged, this,
            [this](const stitch::PreviewStitchConfig &cfg) {
                updatePreviewMenu(cfg);
                QMetaObject::invokeMethod(m_controller, [ctrl = m_controller, cfg] {
                    ctrl->setPreviewConfig(cfg);
                });
            });
    // A file's stitch settings: its segments carry them to the controller,
    // which re-opens its stitch. A seam fine-tune is only valid for the guards
    // it was measured with, so the file may need measuring again.
    connect(m_timeline, &Timeline::clipStitchChanged, this, [this](int id) {
        if (m_rebindingModel)
            return; // a load / undo: the rebind after it covers this
        const ClipItem *c = m_timeline->clipById(id);
        if (!c)
            return;
        pushTimelineToController();
        pushSeamCorrection();
        m_measurements->forgetSeamFailure(c->sourcePath);
        m_measurements->schedule();
        updateSeamStatus();
        if (id == editClipId())
            syncStitchingPanel();
    });
    connect(m_timeline, &Timeline::clipStabilizationChanged, this,
            [this](int id, const insv::StabConfig &) {
                if (m_rebindingModel)
                    return; // a load / undo: the rebind after it covers this
                if (id == editClipId())
                    syncStabilizationPanel();
                // The stab matrix is applied at present time, but the per-lens
                // capture-time correction is baked into stitched frames, so
                // re-decode in place when it toggles with stabilization.
                const ClipItem *c = m_timeline->clipById(id);
                const bool rsOn = c && StabilizerCache::captureTimingOn(*c);
                const bool rsToggled = m_captureTimingOn.value(id, false) != rsOn;
                m_captureTimingOn[id] = rsOn;
                ensureStabilizers();
                if (id == m_clipId) {
                    applyStabilization(m_lastPositionMs);
                    updateSyncStatus();
                }
                if (rsToggled) {
                    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                              Q_ARG(qint64, m_lastTimelineMs));
                    // The privacy blur was found in one of the two stitches.
                    syncPrivacyPanel();
                    qint64 from = 0, to = 0;
                    if (c && m_timeline->shownSourceRange(c->id, from, to) &&
                        !m_privacy.staleness(*c, from, to, false).isEmpty())
                        m_editToolbar->showNotice(
                            tr("The privacy blur was found with the stabilization %1; it may sit "
                               "off the plates now. See the Privacy panel.")
                                .arg(rsOn ? tr("off") : tr("on")),
                            true);
                }
            });
}

void MainWindow::connectMeasurements()
{
    // A measured sync fine-tune for the clip under the playhead: rebuild the
    // stabilizer, and re-stitch in place since the capture-time correction is
    // baked into stitched frames. While playing, both wait for the pause: the
    // re-stitch is a seek, which would stall playback for a few hundred ms.
    connect(m_timeline, &Timeline::clipSyncChanged, this, [this](int id, double) {
        if (m_playing) {
            m_measuredWhilePlaying = true;
            return;
        }
        const ClipItem *c = m_timeline->clipById(id);
        const ClipItem *cur = m_timeline->clipById(m_clipId);
        if (!c || !cur || c->sourcePath != cur->sourcePath)
            return;
        ensureStabilizers();
        applyStabilization(m_lastPositionMs);
        updateSyncStatus();
        QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                  Q_ARG(qint64, m_lastTimelineMs));
    });
    // A measured seam fine-tune: hand it to the stitch and re-stitch the frame
    // on screen if it's this clip's source.
    connect(m_timeline, &Timeline::clipSeamChanged, this, [this](int id) {
        updateSeamStatus();
        if (m_playing) {
            m_measuredWhilePlaying = true;
            return;
        }
        pushSeamCorrection();
        const ClipItem *c = m_timeline->clipById(id);
        const ClipItem *cur = m_timeline->clipById(m_clipId);
        if (c && cur && c->sourcePath == cur->sourcePath)
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs));
    });
}

void MainWindow::ensureStabilizers()
{
    // The current clip's first: used last, so the next one's can't evict it.
    const int idx = m_timeline->indexOfId(m_clipId);
    auto ensure = [this](const ClipItem &c) {
        return m_stabilizers.ensure(c, m_timeline->sourceSyncAdjust(c.sourcePath));
    };
    bool currentBuilt = false;
    if (idx >= 0)
        currentBuilt = ensure(m_timeline->clipAt(idx));
    if (idx >= 0 && idx + 1 < m_timeline->count())
        ensure(m_timeline->clipAt(idx + 1)); // ready for the cut
    publishStabilizer();
    if (currentBuilt) {
        applyStabilization(m_lastPositionMs); // the frame on screen, if paused
        updateSyncStatus();
    }
}

bool MainWindow::seamCorrectionFor(const QString &clipPath, insv::SeamCorrection &seam) const
{
    return m_timeline->sourceSeamCorrection(clipPath, seam);
}

void MainWindow::pushSeamCorrection()
{
    for (int i = 0; i < m_timeline->count(); ++i) {
        const QString &path = m_timeline->clipAt(i).sourcePath;
        insv::SeamCorrection seam;
        seamCorrectionFor(path, seam);
        m_controller->setSeamCorrection(path, seam);
    }
}

void MainWindow::updateSeamStatus()
{
    m_stitchingPanel->setSeamStatus(m_measurements->seamStatus(m_timeline->clipById(m_clipId)));
}

void MainWindow::updateSyncStatus()
{
    m_stabilizationPanel->setSyncStatus(
        m_measurements->syncStatus(m_timeline->clipById(m_clipId)));
}

void MainWindow::publishStabilizer()
{
    m_controller->setCaptureTimings(m_stabilizers.publish(*m_timeline));
}

void MainWindow::applyStabilization(qint64 sourceMs)
{
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (const ClipItem *c = m_timeline->clipById(m_clipId))
        m_stabilizers.correctionFor(*c, m_timeline->sourceSyncAdjust(c->sourcePath), sourceMs, m);
    m_viewport->SetStabilization(&m[0][0]);
}

void MainWindow::syncStabilizationPanel()
{
    const ClipItem *c = m_timeline->clipById(editClipId());
    m_stabilizationPanel->setNotApplicable(
        c && c->flat ? tr("A flat video has no motion data to stabilize with.") : QString());
    if (!c || c->flat) {
        m_stabilizationPanel->setNoClip();
        return;
    }
    using M = StabilizationPanel::Mode;
    const insv::StabConfig &s = c->stabilization;
    M mode = M::Off;
    if (s.mode == insv::StabMode::LeanFollow)
        mode = M::LeanFollow;
    else if (s.mode == insv::StabMode::HorizonLock)
        mode = M::HorizonLock;
    else if (s.mode == insv::StabMode::Custom)
        mode = M::Custom;
    else if (s.mode == insv::StabMode::DirectionLock)
        mode = M::DirectionLock;
    m_stabilizationPanel->setState(
        mode, mode != M::Off ? s : insv::StabConfig::preset(insv::StabMode::LeanFollow));

    // Where the held direction is: its time on the timeline, or in the file
    // when the clip's trim leaves it out.
    QString where;
    if (s.lockDirection) {
        const qint64 sourceMs = std::llround(double(s.directionSec) * 1000.0);
        const int index = m_timeline->indexOfId(c->id);
        if (sourceMs >= c->trimInMs && sourceMs <= c->trimOutMs && index >= 0)
            where = tr("Holds the direction the camera faced at %1.")
                        .arg(PlayerBar::formatTime(
                            m_timeline->clipToTimeline(index, c->localAtSourceMs(sourceMs)),
                            m_playerBar->frameRate()));
        else
            where = tr("Holds the direction the camera faced %1 into the clip's file, outside "
                       "the part on the timeline.")
                        .arg(PlayerBar::formatTime(sourceMs, 0.0));
    }
    m_stabilizationPanel->setDirectionStatus(where);
}

float MainWindow::directionSecAtPlayhead(const ClipItem &c) const
{
    const Timeline::Point p = m_timeline->timelineToClip(m_lastTimelineMs);
    if (p.clipIndex >= 0 && m_timeline->clipAt(p.clipIndex).id == c.id)
        return float(c.sourceAtLocalMs(p.clipLocalMs)) / 1000.0f;
    return float(c.trimInMs) / 1000.0f;
}

insv::StabConfig MainWindow::pastedStabilization(insv::StabConfig cfg, const QString &fromSource,
                                                 const ClipItem &target) const
{
    if (cfg.lockDirection && target.sourcePath != fromSource)
        cfg.directionSec = float(target.trimInMs) / 1000.0f;
    return cfg;
}

void MainWindow::syncStitchingPanel()
{
    const ClipItem *c = m_timeline->clipById(editClipId());
    m_stitchingPanel->setNotApplicable(
        c && c->flat ? tr("A flat video isn't stitched: it has one picture, not two.") : QString());
    if (!c || c->flat) {
        m_stitchingPanel->setNoClip();
        return;
    }
    m_stitchingPanel->setState(c->lensGuard, c->depthSeam, m_lensGuards->profiles(c->sourcePath));
}

void MainWindow::updatePreviewMenu(const stitch::PreviewStitchConfig &cfg)
{
    if (!m_previewQuality)
        return; // a project change before the menu is built
    for (QAction *a : m_previewQuality->actions())
        if (a->data().toInt() == int(cfg.quality))
            a->setChecked(true);
    // A width that isn't a preset (a hand-edited .orp): the nearest one.
    QAction *best = nullptr;
    for (QAction *a : m_previewSize->actions())
        if (!best || std::abs(a->data().toInt() - cfg.width) <
                         std::abs(best->data().toInt() - cfg.width))
            best = a;
    if (best)
        best->setChecked(true);
}
