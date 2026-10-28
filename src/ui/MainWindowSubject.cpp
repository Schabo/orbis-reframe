// MainWindow's Track subject (Reframe panel): arming the viewport box,
// running the tracker, and writing the resulting reframe keys. Declarations
// stay in MainWindow.h.

#include "MainWindow.h"

#include <QMessageBox>
#include <QMetaObject>

#include "../core/Log.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"
#include "EditToolbar.h"
#include "ExportRunner.h"
#include "PlaybackController.h"
#include "ReframeExportController.h"
#include "panels/ReframePanel.h"
#include "panels/SplitScreenPanel.h"
#ifdef ORBIS_REFRAME_HAVE_TRACKING
#include "SubjectTrackController.h" // + track::seedFromViewBox, reduceToKeys
#endif

bool MainWindow::trackingRunning() const
{
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    return m_tracker && m_tracker->running();
#else
    return false;
#endif
}

void MainWindow::setTrackSelecting(bool on)
{
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    if (on && m_plateSelecting)
        setPlateSelecting(false);
    const ClipItem *playing = m_timeline->clipById(m_clipId);
    if (on && (!playing || playing->flat || trackingRunning() || m_exports->exporting()))
        on = false; // a flat clip isn't reframed: nothing to track with
    m_trackSelecting = on;
    m_reframePanel->setTrackSelecting(on);
    m_viewport->SetBoxSelectMode(on);
    if (on) {
        // A still frame to draw on.
        QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
        m_viewport->ClearMarker();
        m_viewport->requestActivate(); // so Esc reaches it
        m_editToolbar->showNotice(tr("Drag a box around the subject to track (Esc cancels)."), true);
    } else {
        if (!trackingRunning())
            m_viewport->ClearMarker();
        m_editToolbar->showNotice(QString());
    }
#else
    Q_UNUSED(on);
#endif
}

void MainWindow::startSubjectTracking(const QRectF &box)
{
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    m_trackSelecting = false;
    m_reframePanel->setTrackSelecting(false);
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c || trackingRunning() || m_exports->exporting()) {
        m_viewport->ClearMarker();
        return;
    }
    const qint64 from = m_lastPositionMs, to = c->trimOutMs;
    if (to - from < 500) {
        m_viewport->ClearMarker();
        m_editToolbar->showNotice(tr("Too close to the end of the clip to track."));
        return;
    }

    // The box -> the subject's direction and angular size, in the stabilized
    // frame the reframe keys live in (the viewport's pose is applied before
    // the stab matrix, so the pose alone maps the box).
    // The active pane's: the box is drawn in it, its view is what the
    // tracker steers.
    const float aspect = m_viewport->ViewAspect();
    const view::Pose shown = m_viewport->Pose();
    const ReframePose look = shown;
    SubjectTrackController::Run run;
    subjecttrack::Request &req = run.req;
    const view::Lens lens = view::lens(shown, aspect);
    req.seed = track::seedFromViewBox(box.x(), box.y(), box.width(), box.height(), shown.yaw,
                                      shown.pitch, shown.roll, lens.fovY, lens.distortion,
                                      aspect);
    const ReframeExportController::ClipJob job = exportJobFor(*c); // the stab, as exported
    req.clipPath = c->sourcePath.toStdString();
    req.fromMs = from;
    req.toMs = to;
    req.fps = c->fps;
    req.stab = job.stab;
    req.seamCorrection = job.seamCorrection;
    req.blur = job.blur;
    req.lensGuard = job.lensGuard;
    run.clipId = c->id;
    run.pane = paneOf(*c);
    run.look = look;
    run.aspect = aspect;

    // It plays what it tracks in the viewport instead: pause playback, which
    // would push frames too (and its own NativeStitchSource would contend).
    QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
    m_tracker->start(run);
    m_reframePanel->setTrackRunning(true);
    m_splitScreenPanel->setEditable(false);
    m_reframePanel->setTrackStatus(tr("Starting…"));
    m_editToolbar->showNotice(tr("Tracking. Space, Esc or Stop tracking ends it."), true);
    updateScreenshotEnabled();
#else
    Q_UNUSED(box);
#endif
}

#ifdef ORBIS_REFRAME_HAVE_TRACKING
void MainWindow::finishSubjectTracking(const subjecttrack::Result &r)
{
    const SubjectTrackController::Run &run = m_tracker->run();
    const int clipId = run.clipId;
    const qint64 from = run.req.fromMs;
    const ReframePose look = run.look;
    m_reframePanel->setTrackRunning(false);
    m_splitScreenPanel->setEditable(true);
    updateScreenshotEnabled();
    core::log::Info("[track] %zu frames, tracker %.1f fps%s%s\n", r.samples.size(),
                    r.fpsAchieved, r.endedLost ? ", ended lost" : "",
                    r.stopped ? ", stopped" : "");

    // Back to normal playback at the last frame tracked -- where to
    // track on from.
    const int idx = m_timeline->indexOfId(clipId);
    auto settle = [this, idx](qint64 sourceMs) {
        if (idx < 0)
            return;
        const ClipItem &clip = m_timeline->clipAt(idx);
        navigateTo(m_timeline->clipToTimeline(idx, clip.localAtSourceMs(sourceMs)), true);
    };
    if (!r.error.empty()) {
        m_editToolbar->showNotice(QString());
        settle(from);
        QMessageBox::warning(this, tr("Track subject"),
                             tr("Tracking failed: %1").arg(QString::fromStdString(r.error)));
        return;
    }
    if (r.samples.size() < 2) {
        settle(from);
        m_editToolbar->showNotice(
            tr("Lost the subject straight away. Try again with a box that fits it closely."));
        return;
    }
    const std::vector<track::PathKey> keys = track::reduceToKeys(r.samples, run.req.fps);
    QList<ReframeKey> list;
    for (const track::PathKey &k : keys)
        list.append(ReframeKey{
            k.tMs, {float(k.yaw), float(k.pitch), look.roll, look.fov, look.distortion}});
    const qint64 until = r.samples.back().tMs;
    m_timeline->replaceClipReframeSpan(clipId, from, until, list, run.pane); // one undo step
    settle(until);
    const QString secs = QString::number((until - from) / 1000.0, 'f', 1);
    if (r.endedLost)
        m_editToolbar->showNotice(
            tr("Tracked %1 s (%2 keyframes), up to where the subject was last seen -- "
               "the playhead is there to track on from.")
                .arg(secs)
                .arg(list.size()),
            true);
    else
        m_editToolbar->showNotice(
            tr("Tracked %1 s (%2 keyframes).").arg(secs).arg(list.size()));
}
#endif
