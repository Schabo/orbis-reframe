// MainWindow's Privacy tab: plate blur flag, scan / follow jobs, the picture
// context menu that removes or restores a region, and the stale-blur confirm
// used before export and screenshots.

#include "MainWindow.h"

#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "../core/Log.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"
#include "EditToolbar.h"
#include "ExportRunner.h"
#include "PlaybackController.h"
#include "SystemCheck.h"
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
#include "PlateJobController.h"
#endif
#include "panels/PrivacyPanel.h"

void MainWindow::connectPrivacy()
{
    // Privacy panel: the flag goes onto the clip being edited; a change
    // re-sends playback's blur regions and re-stitches the paused frame.
    connect(m_privacyPanel, &PrivacyPanel::blurToggled, this, [this](bool on) {
        if (const int id = editClipId())
            m_timeline->setClipBlurPlates(id, on);
    });
    connect(m_privacyPanel, &PrivacyPanel::scanRequested, this, &MainWindow::startPlateScan);
    connect(m_privacyPanel, &PrivacyPanel::plateButtonClicked, this,
            &MainWindow::togglePlateBlur);
    connect(m_privacyPanel, &PrivacyPanel::restoreRemovedRequested, this, [this] {
        if (const int id = editClipId())
            m_timeline->setClipBlurRemovals(id, {});
    });
    connect(m_viewport, &EquirectViewportWindow::pictureContextMenu, this,
            &MainWindow::showPictureMenu);
    connect(m_privacyPanel, &PrivacyPanel::scanStopRequested, this, &MainWindow::stopPlateJob);
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    m_plateJobs = new PlateJobController(this);
    connect(m_plateJobs, &PlateJobController::scanProgress, this, [this](qint64 tMs, double fps) {
        const qint64 from = m_plateJobFromMs, to = m_plateJobToMs;
        const int pct = static_cast<int>(100 * (tMs - from) / std::max<qint64>(1, to - from));
        m_privacyPanel->setScanStatus(tr("Scanning… %1% (%2 fps). Stop keeps the part scanned.")
                                          .arg(std::clamp(pct, 0, 100))
                                          .arg(fps, 0, 'f', 1));
    });
    connect(m_plateJobs, &PlateJobController::scanFinished, this,
            [this](std::shared_ptr<const platescan::Result> r) { finishPlateScan(*r); });
    connect(m_plateJobs, &PlateJobController::followProgress, this, [this](qint64 tMs, bool backward) {
        const int idx = m_timeline->indexOfId(m_plateJobClipId);
        if (idx >= 0)
            showPlayhead(
                m_timeline->clipToTimeline(idx, m_timeline->clipAt(idx).localAtSourceMs(tMs)));
        const qint64 from = m_plateJobFromMs;
        m_privacyPanel->setScanStatus(
            backward ? tr("Following the plate back… %1 s before").arg((from - tMs) / 1000.0, 0, 'f', 1)
                     : tr("Following the plate… %1 s").arg((tMs - from) / 1000.0, 0, 'f', 1));
    });
    connect(m_plateJobs, &PlateJobController::followFinished, this,
            [this](std::shared_ptr<const platescan::FollowResult> r) { finishPlateFollow(*r); });
#endif
    connect(m_privacyPanel, &PrivacyPanel::showRegionsToggled, this, [this](bool on) {
        m_showPlateRegions = on;
        pushBlur();
        if (!m_playing)
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs));
    });
    connect(m_timeline, &Timeline::clipPrivacyChanged, this, [this](int, bool) {
        pushBlur();
        syncPrivacyPanel();
        if (!m_playing)
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs));
    });
    connect(m_timeline, &Timeline::clipTrimChanged, this, [this] { syncPrivacyPanel(); });
}

bool MainWindow::confirmStalePrivacy(const QString &title, const QStringList &warnings)
{
    if (warnings.isEmpty())
        return true;
    QMessageBox box(QMessageBox::Warning, title,
                    tr("The privacy blur may be out of date and needs checking or redoing."),
                    QMessageBox::Cancel, this);
    box.setInformativeText(warnings.join(QStringLiteral("\n\n")));
    QPushButton *go = box.addButton(tr("Continue anyway"), QMessageBox::AcceptRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == go;
}

void MainWindow::pushBlur()
{
    m_privacy.pushTo(*m_controller, *m_timeline, m_showPlateRegions);
}

void MainWindow::showPictureMenu(const QPoint &globalPos, const std::array<float, 3> &dir)
{
    // The clip on screen and the frame shown (paused: the playhead's).
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c)
        return;
    const qint64 t = m_lastPositionMs;
    const int clipId = c->id;
    const PrivacyBlur::Hit hit = m_privacy.hitAt(*c, t, dir.data(), m_showPlateRegions);
    if (hit.added >= 0) {
        QMenu menu;
        auto edit = [this, clipId, ai = hit.added](qint64 fromMs) {
            const ClipItem *c = m_timeline->clipById(clipId);
            if (c && ai < int(c->blurAdditions.size()))
                m_timeline->setClipBlurAdditions(clipId, PrivacyBlur::cutAddition(*c, ai, fromMs));
        };
        menu.addAction(tr("Remove this blur"), this, [edit] { edit(INT64_MIN); })
            ->setToolTip(tr("A plate blurred by hand: drop it in every frame."));
        menu.addAction(tr("Remove this blur from here on"), this, [edit, t] { edit(t - 1); });
        menu.exec(globalPos);
        return;
    }
    if (!hit.region)
        return;
    const int32_t track = hit.track;
    QMenu menu;
    if (hit.removed) {
        menu.addAction(tr("Restore this blur"), this, [this, clipId, track, t] {
            const ClipItem *c = m_timeline->clipById(clipId);
            if (const auto keep = c ? m_privacy.withRestored(*c, track, t) : std::nullopt)
                m_timeline->setClipBlurRemovals(clipId, *keep);
        });
    } else {
        auto remove = [this, clipId, track](qint64 fromMs) {
            const ClipItem *c = m_timeline->clipById(clipId);
            const auto all = c ? m_privacy.withRemoved(*c, track, fromMs) : std::nullopt;
            if (!all)
                return;
            m_timeline->setClipBlurRemovals(clipId, *all);
            m_editToolbar->showNotice(tr("Blur removed. Right-click it with Show regions on to "
                                       "restore it, or undo."));
        };
        menu.addAction(tr("Remove this blur"), this, [remove] { remove(INT64_MIN); })
            ->setToolTip(tr("A false hit: drop this region in every frame of its track."));
        menu.addAction(tr("Remove this blur from here on"), this, [remove, t] { remove(t); });
    }
    menu.exec(globalPos);
}

void MainWindow::syncPrivacyPanel()
{
    const ClipItem *c = m_timeline->clipById(editClipId());
    m_privacyPanel->setNotApplicable(
        c && c->flat ? tr("The number-plate blur works on 360° clips only for now.") : QString());
    if (!c || c->flat) {
        m_privacyPanel->setNoClip();
        return;
    }
    const QString cantScan = syscheck::plateScanUnavailableReason();
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const bool canScan = cantScan.isEmpty() && !c->mediaMissing && !plateJobRunning();
#else
    const bool canScan = false;
#endif
    // What shows of it: the trim, and past it where a transition plays on.
    qint64 from = c->trimInMs, to = c->trimOutMs;
    m_timeline->shownSourceRange(c->id, from, to);
    QString status = m_privacy.status(*c, from, to);
    if (!cantScan.isEmpty())
        status += QLatin1Char(' ') + cantScan;
    m_privacyPanel->setState(c->blurPlates, status, canScan, static_cast<int>(c->blurRemovals.size()));
    m_privacyPanel->setWarning(m_privacy.staleness(*c, from, to, false));
}

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
platescan::Request MainWindow::plateRequestFor(const ClipItem &c)
{
    // The stitch as playback / export set it up for this clip.
    platescan::Request req;
    req.clipPath = c.sourcePath.toStdString();
    qint64 from = c.trimInMs, to = c.trimOutMs;
    m_timeline->shownSourceRange(c.id, from, to); // with a transition's footage past the trims
    req.fromMs = from;
    req.toMs = to;
    req.fps = c.fps;
    req.syncAdjustMs = std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs;
    seamCorrectionFor(c.sourcePath, req.seamCorrection);
    req.lensGuard = c.lensGuard;
    req.depthSeam = c.depthSeam;
    req.captureTiming = StabilizerCache::captureTimingOn(c);
    return req;
}
#endif

void MainWindow::setPlateSelecting(bool on)
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (on && (!c || c->flat || c->mediaMissing || plateJobRunning() || trackingRunning() ||
               m_exports->exporting()))
        on = false;
    if (on && m_trackSelecting)
        setTrackSelecting(false);
    m_plateSelecting = on;
    m_privacyPanel->setPlateState(on ? PrivacyPanel::PlateState::Selecting : PrivacyPanel::PlateState::Idle);
    m_viewport->SetBoxSelectMode(on);
    if (on) {
        // A still frame to draw on.
        QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
        m_viewport->ClearMarker();
        m_viewport->requestActivate(); // so Esc reaches it
        m_editToolbar->showNotice(tr("Drag a box around the plate to blur (Esc cancels). It is "
                                   "followed from this frame on, then back from it."),
                                true);
    } else {
        m_viewport->ClearMarker();
        m_editToolbar->showNotice(QString());
    }
#else
    Q_UNUSED(on);
#endif
}

void MainWindow::togglePlateBlur()
{
    if (plateFollowing())
        stopPlateJob();
    else
        setPlateSelecting(!m_plateSelecting);
}

void MainWindow::startPlateFollow(const QRectF &box)
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    m_plateSelecting = false;
    m_privacyPanel->setPlateState(PrivacyPanel::PlateState::Idle);
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c || c->mediaMissing || plateJobRunning() || trackingRunning() || m_exports->exporting()) {
        m_viewport->ClearMarker();
        return;
    }
    const qint64 from = m_lastPositionMs, to = c->trimOutMs, back = c->trimInMs;
    if (to - from < 100 && from - back < 100) {
        m_viewport->ClearMarker();
        m_editToolbar->showNotice(tr("The clip is too short."));
        return;
    }
    // The box's corners on the sphere, through the frame on screen.
    insv::BlurQuad seed;
    const double xs[4] = {box.left(), box.right(), box.right(), box.left()};
    const double ys[4] = {box.top(), box.top(), box.bottom(), box.bottom()};
    for (int k = 0; k < 4; ++k)
        m_viewport->SphereDirAtFraction(xs[k], ys[k], seed.corner[k]);
    platescan::Request req = plateRequestFor(*c);
    req.fromMs = from;

    QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
    m_plateJobClipId = c->id;
    m_plateJobFromMs = from;
    m_plateJobs->startFollow(req, seed, back);
    m_privacyPanel->setPlateState(PrivacyPanel::PlateState::Running);
    m_privacyPanel->setScanStatus(tr("Following the plate…"));
    m_editToolbar->showNotice(tr("Following the plate. Stop following in the Privacy panel ends it."), true);
    syncPrivacyPanel();
#else
    Q_UNUSED(box);
#endif
}

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
void MainWindow::finishPlateFollow(const platescan::FollowResult &r)
{
    m_privacyPanel->setPlateState(PrivacyPanel::PlateState::Idle);
    m_viewport->ClearMarker();
    const auto &frames = r.follow.frames;
    core::log::Info("[plates] follow: %zu frames, %d detected%s%s%s\n", frames.size(),
                    r.follow.detections, r.follow.lost ? ", lost" : "",
                    r.lostBackward ? ", lost backwards" : "", r.stopped ? ", stopped" : "");
    syncPrivacyPanel();
    const int clipId = m_plateJobClipId;
    const qint64 from = m_plateJobFromMs;
    const int idx = m_timeline->indexOfId(clipId);
    auto settle = [this, idx](qint64 sourceMs) {
        if (idx >= 0)
            navigateTo(m_timeline->clipToTimeline(idx, m_timeline->clipAt(idx).localAtSourceMs(sourceMs)),
                       true);
    };
    // A failed backward pass keeps the forward one (and says so below).
    if (frames.empty() || idx < 0) {
        m_editToolbar->showNotice(QString());
        settle(from);
        if (!r.error.empty())
            QMessageBox::warning(this, tr("Blur a plate"),
                                 tr("Following the plate failed: %1").arg(QString::fromStdString(r.error)));
        return;
    }
    // The playhead first: the change re-shows the paused frame there --
    // where it was lost going forwards, else going backwards, else the
    // end.
    const qint64 first = frames.front().tMs, until = frames.back().tMs;
    const bool lostBackOnly = r.lostBackward && !r.follow.lost;
    settle(lostBackOnly ? first : until);
    std::vector<privacy::BlurAddition> all = m_timeline->clipAt(idx).blurAdditions;
    all.push_back(platescan::toBlurAddition(r));
    m_timeline->setClipBlurAdditions(clipId, all); // one undo step
    const QString secs = QString::number((until - first) / 1000.0, 'f', 1);
    const QString before = QString::number((from - first) / 1000.0, 'f', 1);
    if (r.follow.lost || r.lostBackward)
        m_editToolbar->showNotice(
            tr("Blurred the plate for %1 s (%2 s of it before the box), up to where it was lost "
               "-- the playhead is there. Blur a plate again to pick it up.")
                .arg(secs, before),
            true);
    else
        m_editToolbar->showNotice(tr("Blurred the plate for %1 s (%2 s of it before the box).")
                                    .arg(secs, before));
    if (!r.error.empty())
        QMessageBox::warning(this, tr("Blur a plate"),
                             tr("Following the plate failed part of the way: %1")
                                 .arg(QString::fromStdString(r.error)));
}
#endif

void MainWindow::startPlateScan()
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const ClipItem *c = m_timeline->clipById(editClipId());
    if (plateJobRunning() || !c || c->mediaMissing)
        return;
    if (m_plateSelecting)
        setPlateSelecting(false);
    m_plateJobSource = c->sourcePath;
    m_plateJobClipId = c->id;
    const platescan::Request req = plateRequestFor(*c);
    m_plateJobFromMs = req.fromMs;
    m_plateJobToMs = req.toMs;
    m_plateJobs->startScan(req);
    m_privacyPanel->setScanRunning(true);
    m_privacyPanel->setScanStatus(tr("Starting…"));
    syncPrivacyPanel();
#endif
}

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
void MainWindow::finishPlateScan(const platescan::Result &r)
{
    const QString source = m_plateJobSource;
    m_privacyPanel->setScanRunning(false);
    core::log::Info("[plates] %s: %zu frames, %d plates, %.1f fps on %s%s\n",
                    source.toUtf8().constData(), r.scan.frames.size(), r.scan.plates,
                    r.fpsAchieved, r.device.c_str(), r.stopped ? ", stopped" : "");
    if (!r.error.empty()) {
        syncPrivacyPanel();
        QMessageBox::warning(this, tr("Scan for plates"),
                             tr("The scan failed: %1").arg(QString::fromStdString(r.error)));
        return;
    }
    const ClipItem *c = m_timeline->clipById(m_plateJobClipId);
    if (!r.scan.empty() && (!c || c->sourcePath != source)) {
        core::log::Info("[plates] the scanned clip is gone; the scan is dropped\n");
    } else if (!r.scan.empty()) {
        // A new scan of the clip's own, from the one it had: a copy or a
        // split half that shared it keeps that one. Undo goes back to it.
        privacy::PlateScan scan;
        if (const PlateScanCache::Scan had = m_privacy.scanOf(*c))
            scan = *had;
        scan.merge(r.scan);
        const QString key = PlateScanCache::newKey();
        QString err;
        if (!m_privacy.scans().store(source, key, scan, &err))
            QMessageBox::warning(this, tr("Scan for plates"),
                                 tr("The scan couldn't be saved (%1); it holds until the "
                                    "app closes.")
                                     .arg(err));
        m_timeline->setClipPlateScan(c->id, key);
    }
    pushBlur();
    syncPrivacyPanel();
    if (!m_playing)
        QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                  Q_ARG(qint64, m_lastTimelineMs));
    m_editToolbar->showNotice(r.stopped ? tr("Plate scan stopped; the part scanned is kept.")
                                      : tr("Plate scan done."));
}
#endif

bool MainWindow::plateJobRunning() const
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    return m_plateJobs && m_plateJobs->running();
#else
    return false;
#endif
}

bool MainWindow::plateFollowing() const
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    return m_plateJobs && m_plateJobs->following();
#else
    return false;
#endif
}

void MainWindow::stopPlateJob()
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    if (m_plateJobs)
        m_plateJobs->stop();
#endif
}
