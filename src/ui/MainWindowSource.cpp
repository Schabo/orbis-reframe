// MainWindow's preview of a file from the Media tab: the file plays in the
// viewport in the timeline's place, with its own sound, so takes can be
// watched and looked around in before they go on the timeline. Playback
// keeps the timeline meanwhile (PlaybackController / AudioPlayer
// showSource) and shows it again, where it was, when the preview ends.

#include "MainWindow.h"

#include <QApplication>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

#include "AudioPlayer.h"
#include "ClipFactsCache.h"
#include "InsetFeed.h"
#include "MediaPoolPanel.h"
#include "PlaybackController.h"
#include "PlayerBar.h"
#include "RenderJobs.h"
#include "TimelineWidget.h"
#include "panels/MediaInfoPanel.h"
#include "../core/MediaPool.h"
#include "../core/Project.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

namespace {

// A flat video's or an audio file's stand-in picture rate when the file
// doesn't say (the length is what matters: the picture is a layer, or none).
constexpr double kDefaultSourceFps = 30.0;

} // namespace

void MainWindow::previewMedia(const QString &path)
{
    if (path.isEmpty())
        return;
    if (path == m_sourcePath) {
        QMetaObject::invokeMethod(m_controller, "togglePlay", Qt::QueuedConnection);
        return;
    }
    if (showSource(path))
        QMetaObject::invokeMethod(m_controller, "play", Qt::QueuedConnection);
}

bool MainWindow::showSource(const QString &path)
{
    const MediaItem *m = m_project->mediaPool()->item(path);
    if (!m || m->missing || m->durationMs <= 0)
        return false;
    // A box being drawn is for the timeline's picture.
    if (m_trackSelecting)
        setTrackSelecting(false);
    if (m_plateSelecting)
        setPlateSelecting(false);

    // The file as it would play on a timeline of its own: its sound (and a
    // flat video's picture, a full-frame layer) from that timeline, the way
    // the main timeline's are made.
    Timeline own;
    PlaybackController::Segment seg;
    seg.sourcePath = path;
    seg.outMs = m->durationMs;
    seg.sourceDurationMs = m->durationMs;
    seg.fps = m->fps > 0.0 ? m->fps : kDefaultSourceFps;
    std::vector<InsetFeed::Clip> insets;
    QList<qint64> marks;
    std::optional<ClipItem> clip;
    switch (m->kind) {
    case MediaItem::Kind::Clip360: {
        // Stitched as it would be if it went on at the end of the timeline:
        // with its other clips' stitch settings when it's there already.
        const stitch::LensGuardProfiles guards = m_clipFacts->profiles(path);
        // Its stabilization too: the nearest 360 clip's, as it would be
        // there (Off with none).
        ClipItem c = m_timeline->newClip(m_timeline->count(), path, m->durationMs, m->fps,
                                         m->width, m->height, false, guards);
        c.id = PlaybackController::kSourceClipId;
        clip = c;
        seg.lensGuard = c.lensGuard;
        seg.stitchMethod = c.stitchMethod;
        seg.lensCalibration = c.lensCalibration;
        seg.seamColourBlend = c.seamColourBlend;
        seg.seamFlow = c.seamFlow;
        own.appendClip(path, m->durationMs, m->fps, m->width, m->height, audioStreamCount(path),
                       false, guards);
        marks = m_clipFacts->highlights(path);
        break;
    }
    case MediaItem::Kind::Video: {
        seg.flat = true;
        own.appendClip(path, m->durationMs, m->fps, m->width, m->height, audioStreamCount(path),
                       true);
        const OutputSettings &out = m_project->output();
        insets = renderjobs::insetFeed(own, out.width, out.height, m_privacy);
        break;
    }
    case MediaItem::Kind::Audio:
        seg.flat = true; // no picture
        own.addAudioClip(own.addTrack(Track::Kind::Audio), path, m->durationMs, 0);
        break;
    }

    if (!previewing())
        m_sourceSavedPose = m_viewport->Pose();
    m_sourcePath = path;
    m_sourceFps = seg.fps;
    m_sourceDurationMs = m->durationMs;
    m_sourceMarks = marks;
    m_sourcePositionMs = 0;
    m_sourceClip = clip;
    ensureStabilizers(); // before its first frame is shown
    QMetaObject::invokeMethod(m_controller, [ctrl = m_controller, seg, insets] {
        ctrl->showSource(seg, insets);
    });
    m_audioPlayer->showSource(seg, renderjobs::mixClips(own));
    m_playerBar->setSource(QFileInfo(path).fileName());
    m_playerBar->setFrameRate(seg.fps);
    m_playerBar->setCuts({});
    m_mediaPanel->setPreview(path, false);
    showMediaInfo(path, true);
    updateScreenshotEnabled();
    return true;
}

void MainWindow::leaveSource()
{
    if (!previewing())
        return;
    m_sourcePath.clear();
    m_sourceMarks.clear();
    m_sourceClip.reset();
    publishStabilizer();
    QMetaObject::invokeMethod(m_controller, &PlaybackController::showTimeline);
    m_audioPlayer->showTimeline();
    // The view the timeline was left with; its frames bring their own
    // stabilization, grade and panes again.
    m_viewport->SetPose(m_sourceSavedPose);
    applyStabilization(m_lastPositionMs);
    m_playerBar->setSource(QString());
    updatePlayerBar();
    m_mediaPanel->setPreview(QString(), false);
    if (const ClipItem *c = m_timeline->clipById(editClipId()))
        syncMediaInfoPanel(*c);
    else
        m_mediaInfoPanel->setNoClip();
    updateScreenshotEnabled();
    // Space in the Media list previews its file: back on the timeline, it
    // plays the timeline.
    if (m_mediaPanel->isAncestorOf(QApplication::focusWidget()))
        m_timelineWidget->setFocus();
}

void MainWindow::seekSource(qint64 sourceMs, bool pause)
{
    sourceMs = std::clamp<qint64>(sourceMs, 0, m_sourceDurationMs);
    if (pause && m_playing)
        QMetaObject::invokeMethod(m_controller, "pause", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_controller, "seekSource", Qt::QueuedConnection,
                              Q_ARG(qint64, sourceMs));
    m_sourcePositionMs = sourceMs;
    m_playerBar->setPosition(sourceMs);
}

bool MainWindow::stepSource(SourceStep step, int direction)
{
    if (!previewing())
        return false;
    const qint64 at = m_sourcePositionMs;
    switch (step) {
    case SourceStep::Frame: {
        // As Timeline::stepFrames does in a clip: the frame shown at `at`
        // (the one starting last by then), and the start of the one wanted.
        const double period = 1000.0 / m_sourceFps;
        const qint64 k = qint64(std::ceil((at - 1) / period - 1e-6)) + direction;
        seekSource(qint64(std::ceil(double(k) * period - 1e-6)), true);
        break;
    }
    case SourceStep::Second:
        seekSource(at + direction * 1000, false);
        break;
    case SourceStep::Edge:
        seekSource(direction < 0 ? 0 : m_sourceDurationMs, false);
        break;
    case SourceStep::Mark: {
        // A mark within a frame of the playhead is the one it's on.
        const qint64 near = std::llround(1000.0 / m_sourceFps / 2);
        qint64 to = -1;
        for (const qint64 t : m_sourceMarks) {
            if (direction > 0 && t > at + near && (to < 0 || t < to))
                to = t;
            if (direction < 0 && t < at - near && (to < 0 || t > to))
                to = t;
        }
        if (to >= 0)
            seekSource(to, false);
        break;
    }
    case SourceStep::None:
        break;
    }
    return true;
}
