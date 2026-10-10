#include "MainWindow.h"

#include "AboutDialog.h"
#include "SystemCheck.h"

#include "../core/Log.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStandardPaths>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>

#include <InsvAudioDemux.h> // insv::InsvAudioDemux::countAudioStreams
#include <InsvTrailer.h>   // insv::ReadInsvTelemetry
#include <IStitchSource.h> // stitch::PreviewStitchConfig
#include <ProbeClip.h>     // stitch::ProbeClip
#include <FlatVideoReader.h> // exportpipe::FlatVideoReader::probe (overlays)

#ifdef ORBIS_REFRAME_HAVE_AUDIO
#include "AudioPlayer.h"
#endif
#include <ClipSeam.h> // stitch::MeasureClipSeam
#ifdef ORBIS_REFRAME_HAVE_CLIP_SYNC
#include <ClipSync.h> // stitch::MeasureClipSync
#endif
#ifdef ORBIS_REFRAME_HAVE_TRACKING
#include "SubjectTrackController.h" // + track::seedFromViewBox, reduceToKeys
#endif
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
#include "PlateJobController.h"
#endif
#include "ExportOptionsDialog.h"
#include "PlaybackController.h"
#include "ReframeExportController.h"
#include "SettingsSidebar.h"
#include "TimelineWidget.h"
#include "WaveformCache.h"
#include "TransportBar.h"
#include "panels/AudioPanel.h"
#include "panels/ColourPanel.h"
#include "panels/MediaInfoPanel.h"
#include "panels/OverlayPanel.h"
#include "panels/PrivacyPanel.h"
#include "panels/ProjectPanel.h"
#include "panels/ReframePanel.h"
#include "panels/StabilizationPanel.h"
#include "panels/StitchingPanel.h"
#include "../core/ClipItem.h"
#include "../core/Project.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

MainWindow::MainWindow(QVulkanInstance *vulkanInstance, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("orbis-reframe"));

    m_viewport = new EquirectViewportWindow;
    m_viewport->setVulkanInstance(vulkanInstance);
    // Import decoded lens frames from the decoder's Vulkan device
    // (enabled only if supported; the viewport checks before using it).
    m_viewport->setDeviceExtensions({QByteArrayLiteral("VK_KHR_external_memory_fd")});
    // Once Vulkan is up: say if this GPU can't decode video or export.
    // Only the first time -- vulkanReady() repeats after a device loss.
    connect(m_viewport, &EquirectViewportWindow::vulkanReady, this,
            [this] { syscheck::warnAtStartup(this, m_viewport); }, Qt::SingleShotConnection);
    // The shaders write display-ready (gamma-encoded) colour, so the swapchain
    // must not encode it again: an *_SRGB format -- the first one a headless
    // Sway offers -- lifted every dark tone.
    m_viewport->setPreferredColorFormats({VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM});

    QWidget *viewportContainer = QWidget::createWindowContainer(m_viewport, this);
    viewportContainer->setMinimumSize(320, 160);
    viewportContainer->setFocusPolicy(Qt::StrongFocus);
    qApp->installEventFilter(this); // see eventFilter(): release text fields

    m_timelineWidget = new TimelineWidget(this);
    m_waveforms = new WaveformCache(this); // the timeline's waveforms (scanned in the background)
    m_transport = new TransportBar(this);

    // The edit model, owned by the GUI thread: a Project holding a Timeline.
    // The whole timeline is handed to PlaybackController as a segment list
    // (pushTimelineToController); it plays across the clips in timeline time.
    m_project = new Project(this);
    m_timeline = m_project->timeline();
    connect(m_project, &Project::modifiedChanged, this, [this](bool) { updateWindowTitle(); });
    connect(m_project, &Project::filePathChanged, this, [this](const QString &) {
        updateWindowTitle();
    });
    updateWindowTitle();

    // Left: viewport over the timeline over the transport strip.
    auto *leftColumn = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    leftLayout->addWidget(viewportContainer, 1);
    leftLayout->addWidget(m_timelineWidget, 0);
    leftLayout->addWidget(m_transport, 0);

    // Right: the settings sidebar. Each panel is wired to Project /
    // PlaybackController / the clip under the playhead below.
    m_sidebar = new SettingsSidebar(this);
    m_projectPanel = new ProjectPanel;
    m_sidebar->addPanel(m_projectPanel);
    m_stitchingPanel = new StitchingPanel;
    m_sidebar->addPanel(m_stitchingPanel);
    m_reframePanel = new ReframePanel;
    m_sidebar->addPanel(m_reframePanel);
    m_stabilizationPanel = new StabilizationPanel;
    m_sidebar->addPanel(m_stabilizationPanel);
    m_audioPanel = new AudioPanel;
    m_audioPanel->setNoClip(); // nothing open yet
    m_sidebar->addPanel(m_audioPanel);
    m_colourPanel = new ColourPanel;
    m_colourPanel->setNoClip(); // nothing open yet
    m_sidebar->addPanel(m_colourPanel);
    m_privacyPanel = new PrivacyPanel; // starts in its no-clip state
    m_sidebar->addPanel(m_privacyPanel);
    m_overlayPanel = new OverlayPanel; // starts with no overlay clip
    m_sidebar->addPanel(m_overlayPanel);
    m_mediaInfoPanel = new MediaInfoPanel; // starts in its no-clip state
    m_sidebar->addPanel(m_mediaInfoPanel);

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(leftColumn, 1);
    layout->addWidget(m_sidebar, 0);
    setCentralWidget(central);

    // The controller runs on its own thread: opening a clip and its first
    // stitch block, which must not stall the GUI. No parent
    // -- ownership is handed to the thread via the finished/deleteLater wiring
    // below.
    m_controller = new PlaybackController;
    m_controller->setViewport(m_viewport);
    m_controller->setStabilizationProvider(
        [shared = m_stabShared](const QString &path, qint64 sourceMs, float m[9]) {
            std::lock_guard<std::mutex> lk(shared->mx);
            if (shared->off) {
                const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                std::copy(ident, ident + 9, m);
                return true;
            }
            if (!shared->stab || path != shared->path)
                return false; // clip not built yet -- keep the current matrix
            float r[3][3];
            shared->stab->correctionMatrix(sourceMs / 1000.0, r);
            std::copy(&r[0][0], &r[0][0] + 9, m);
            return true;
        });
    m_playbackThread = new QThread(this);
    m_controller->moveToThread(m_playbackThread);
    connect(m_playbackThread, &QThread::finished, m_controller, &QObject::deleteLater);

    // Audio: a slave of m_controller's clock, entirely on the GUI thread --
    // see AudioPlayer's own comment for why. A normal child, unlike
    // m_controller.
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer = new AudioPlayer(this);
#endif

    // Stitching settings: a panel edit updates the Project (persisted to the
    // .orp, marks it modified); a Project change (an edit, or a project load)
    // is reflected back in the panel and pushed to the stitch path on the
    // worker thread, which re-stitches the current frame.
    connect(m_stitchingPanel, &StitchingPanel::configChanged, m_project,
            &Project::setPreviewStitch);
    // Project settings: output aspect / size / fps. The viewport is pinned to
    // the output aspect; export reads the size and rate from the Project when
    // it starts.
    connect(m_projectPanel, &ProjectPanel::outputChanged, m_project, &Project::setOutput);
    connect(m_project, &Project::outputChanged, this, [this](const OutputSettings &out) {
        m_projectPanel->setOutput(out); // setOutput() doesn't re-emit
        m_viewport->SetOutputAspect(static_cast<float>(out.aspectRatio()));
    });
    m_viewport->SetOutputAspect(static_cast<float>(m_project->output().aspectRatio()));
    auto refreshFirstClipFps = [this] {
        m_projectPanel->setFirstClipFps(m_timeline->count() > 0 ? m_timeline->clipAt(0).fps
                                                                : 0.0);
    };
    connect(m_project, &Project::projectReset, this, refreshFirstClipFps);
    connect(m_timeline, &Timeline::clipInserted, this, refreshFirstClipFps);
    connect(m_timeline, &Timeline::clipRemoved, this, refreshFirstClipFps);
    connect(m_timeline, &Timeline::clipsReordered, this, refreshFirstClipFps);
    // Stabilization panel -> Project. All modes drive the native
    // reprojection-time path (Project::stabilization()); the panel hands back
    // the right insv::StabConfig for the mode (LeanFollow window from its
    // Strength slider, Custom from the knobs, Off otherwise).
    connect(m_stabilizationPanel, &StabilizationPanel::modeChanged, this,
            [this](StabilizationPanel::Mode, const insv::StabConfig &nativeCfg) {
                m_project->setStabilization(nativeCfg);
            });
    auto refreshStabPanel = [this] {
        using M = StabilizationPanel::Mode;
        const insv::StabConfig &s = m_project->stabilization();
        M mode = M::Off;
        if (s.mode == insv::StabMode::LeanFollow)
            mode = M::LeanFollow;
        else if (s.mode == insv::StabMode::HorizonLock)
            mode = M::HorizonLock;
        else if (s.mode == insv::StabMode::Custom)
            mode = M::Custom;
        const bool nativeMode = (mode != M::Off);
        m_stabilizationPanel->setState(
            mode, nativeMode ? s : insv::StabConfig::preset(insv::StabMode::LeanFollow));
    };
    connect(m_project, &Project::previewStitchChanged, this,
            [this, refreshStabPanel](const stitch::PreviewStitchConfig &cfg) {
                m_stitchingPanel->setConfig(cfg); // setConfig() doesn't re-emit
                refreshStabPanel();
                // A seam fine-tune is only valid for the guard it was measured
                // with; the controller re-opens its sources for the new config
                // and picks these up.
                pushSeamAdjust();
                m_seamFailed.clear();
                scheduleClipScans();
                updateSeamStatus();
                QMetaObject::invokeMethod(m_controller, [ctrl = m_controller, cfg] {
                    ctrl->setPreviewConfig(cfg);
                });
            });
    connect(m_project, &Project::stabilizationChanged, this,
            [this, refreshStabPanel](const insv::StabConfig &cfg) {
                // The per-lens capture-time correction is baked into stitched
                // frames (unlike the stab matrix, applied at present time), so
                // re-decode in place when it toggles with stabilization.
                auto rsActive = [](const insv::StabConfig &c) {
                    return c.mode != insv::StabMode::Off;
                };
                const bool rsToggled = rsActive(cfg) != rsActive(m_stabConfig);
                m_stabConfig = cfg;
                refreshStabPanel();
                if (const ClipItem *c = m_timeline->clipById(m_clipId)) {
                    ensureStabilizerFor(c->sourcePath);
                    applyStabilization(m_lastPositionMs);
                }
                if (rsToggled) {
                    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                              Q_ARG(qint64, m_lastTimelineMs));
                    // The privacy blur was found in one of the two stitches.
                    syncPrivacyPanel();
                    for (int i = 0; i < m_timeline->count(); ++i) {
                        const ClipItem &ci = m_timeline->clipAt(i);
                        if (!privacyStaleness(ci, ci.trimInMs, ci.trimOutMs, false).isEmpty()) {
                            m_transport->showNotice(
                                tr("The privacy blur was found with the stabilization %1; it may sit "
                                   "off the plates now. See the Privacy panel.")
                                    .arg(captureTimingOn() ? tr("off") : tr("on")),
                                true);
                            break;
                        }
                    }
                }
            });

    // View -> controller. These connections cross the thread boundary, so Qt
    // queues them onto the worker's event loop automatically.
    connect(m_transport, &TransportBar::playPauseClicked,
            m_controller, &PlaybackController::togglePlay);

    // Timeline widget -> controller / model. It works entirely in timeline
    // time.
    connect(m_timelineWidget, &TimelineWidget::seekRequested, this, [this](qint64 timelineMs) {
        QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                  Q_ARG(qint64, timelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
        m_audioPlayer->seek(timelineMs);
#endif
    });
    connect(m_timelineWidget, &TimelineWidget::scrubRequested, this, [this](qint64 timelineMs) {
        QMetaObject::invokeMethod(m_controller, "scrub", Qt::QueuedConnection,
                                  Q_ARG(qint64, timelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
        m_audioPlayer->seek(timelineMs);
#endif
    });
    connect(m_timelineWidget, &TimelineWidget::clipTrimInRequested, m_timeline,
            &Timeline::setClipTrimIn);
    connect(m_timelineWidget, &TimelineWidget::clipTrimOutRequested, m_timeline,
            &Timeline::setClipTrimOut);
    connect(m_timelineWidget, &TimelineWidget::clipMoved, m_timeline, &Timeline::moveClip);
    connect(m_timelineWidget, &TimelineWidget::clipRemoveRequested, m_timeline,
            &Timeline::removeClip);
    connect(m_timelineWidget, &TimelineWidget::clipSplitRequested, this,
            [this](int id, qint64 atSourceMs) { m_timeline->splitClip(id, atSourceMs); });
    connect(m_timelineWidget, &TimelineWidget::clipSpeedRequested, m_timeline,
            &Timeline::setClipSpeed);

    // Extra tracks. The widget edits them through Timeline; any change
    // comes back as tracksChanged, which redraws and re-feeds the mixer.
    connect(m_timelineWidget, &TimelineWidget::trackClipSelected, this,
            &MainWindow::selectTrackClip);
    // Clicking a video clip selects it for the panels and the trim buttons,
    // without moving the playhead; 0 = back to following the playhead.
    connect(m_timelineWidget, &TimelineWidget::clipSelected, this, &MainWindow::selectVideoClip);
    connect(m_timeline, &Timeline::clipRemoved, this, [this](int id) {
        if (id == m_selectedClipId)
            selectVideoClip(0);
    });
    connect(m_timelineWidget, &TimelineWidget::trackClipMoved, m_timeline,
            &Timeline::moveTrackClip);
    connect(m_timelineWidget, &TimelineWidget::trackClipTrimInRequested, m_timeline,
            &Timeline::setTrackClipTrimIn);
    connect(m_timelineWidget, &TimelineWidget::trackClipTrimOutRequested, m_timeline,
            &Timeline::setTrackClipTrimOut);
    connect(m_timelineWidget, &TimelineWidget::trackClipRemoveRequested, m_timeline,
            &Timeline::removeTrackClip);
    connect(m_timelineWidget, &TimelineWidget::audioClipLinkRequested, this,
            [this](int id, bool link) {
                const TrackClip *a = m_timeline->trackClipById(id);
                if (a)
                    m_timeline->linkAudioClip(id, link ? linkTargetFor(*a) : 0);
            });
    connect(m_timelineWidget, &TimelineWidget::trackClipSplitRequested, this,
            [this](int id, qint64 atMs) {
                if (const int right = m_timeline->splitTrackClip(id, atMs))
                    selectTrackClip(right);
            });
    connect(m_timelineWidget, &TimelineWidget::trackMuteToggled, this, [this](int id, bool muted) {
        if (const Track *t = m_timeline->trackById(id))
            m_timeline->setTrackAudio(id, t->gainDb, muted);
    });
    connect(m_timelineWidget, &TimelineWidget::mainMuteToggled, m_timeline,
            &Timeline::setMainAudioMuted);
    connect(m_timelineWidget, &TimelineWidget::trackRemoveRequested, m_timeline,
            &Timeline::removeTrack);
    connect(m_timelineWidget, &TimelineWidget::trackRenameRequested, m_timeline,
            &Timeline::setTrackName);
    connect(m_timelineWidget, &TimelineWidget::trackMoved, m_timeline, &Timeline::moveTrack);
    connect(m_timelineWidget, &TimelineWidget::addClipRequested, this,
            &MainWindow::addClipToTimeline);
    connect(m_timelineWidget, &TimelineWidget::addAudioTrackRequested, this,
            [this] { m_timeline->addTrack(Track::Kind::Audio); });
    connect(m_timelineWidget, &TimelineWidget::addAudioFileRequested, this,
            &MainWindow::addAudioFile);
    connect(m_timelineWidget, &TimelineWidget::audioFilesDropped, this,
            &MainWindow::addAudioFiles);
    // Overlay (video) tracks.
    connect(m_timelineWidget, &TimelineWidget::addVideoTrackRequested, this,
            [this] { m_timeline->addTrack(Track::Kind::Video); });
    connect(m_timelineWidget, &TimelineWidget::addVideoFileRequested, this,
            &MainWindow::addVideoFile);
    connect(m_timelineWidget, &TimelineWidget::videoFilesDropped, this,
            &MainWindow::addVideoFiles);
    // Picking an inset in the view selects its clip; dragging it moves it.
    connect(m_viewport, &EquirectViewportWindow::insetPressed, this,
            [this](int clipId) { selectTrackClip(clipId); });
    connect(m_viewport, &EquirectViewportWindow::insetMoved, this,
            [this](int clipId, float cx, float cy) {
                const TrackClip *c = m_timeline->trackClipById(clipId);
                if (!c)
                    return;
                const qint64 src = c->sourceAtLocalMs(
                    std::clamp<qint64>(m_lastTimelineMs - c->startMs, 0, c->timelineDurationMs()));
                inset::Look l = c->lookAt(src);
                l.centerX = cx;
                l.centerY = cy;
                applyInsetLook(clipId, l);
            });
    // Overlay panel -> the selected overlay clip.
    connect(m_overlayPanel, &OverlayPanel::lookEdited, this, [this](const inset::Look &l) {
        if (const TrackClip *c = selectedOverlayClip())
            applyInsetLook(c->id, l);
    });
    connect(m_overlayPanel, &OverlayPanel::frameEdited, this, [this](const inset::Frame &f) {
        if (const TrackClip *c = selectedOverlayClip())
            m_timeline->setTrackClipInset(c->id, c->look, f);
    });
    connect(m_overlayPanel, &OverlayPanel::timingEdited, this,
            [this](qint64 fadeInMs, qint64 fadeOutMs, qint64 startMs) {
                int trackId = 0;
                const TrackClip *c = selectedOverlayClip(&trackId);
                if (!c)
                    return;
                const int id = c->id;
                m_timeline->setTrackClipFades(id, fadeInMs, fadeOutMs);
                m_timeline->moveTrackClip(id, trackId, startMs);
            });
    connect(m_overlayPanel, &OverlayPanel::addKeyRequested, this,
            &MainWindow::setOverlayKeyAtPlayhead);
    connect(m_overlayPanel, &OverlayPanel::removeKeyRequested, this,
            &MainWindow::deleteOverlayKeyAtPlayhead);
    connect(m_overlayPanel, &OverlayPanel::clearKeysRequested, this,
            &MainWindow::clearOverlayKeys);
    // Waveforms arrive from background scans: redraw.
    connect(m_waveforms, &WaveformCache::ready, this, &MainWindow::refreshTimelineWidget);
    connect(m_timelineWidget, &TimelineWidget::clipDetachAudioRequested, this,
            &MainWindow::detachOtherMic);
    connect(m_timelineWidget, &TimelineWidget::overlaySoundRequested, this,
            &MainWindow::putOverlaySoundOnTrack);
    connect(m_timeline, &Timeline::tracksChanged, this, &MainWindow::onTracksChanged);
    // Audio panel in audio-clip mode -> the selected clip and its track.
    connect(m_audioPanel, &AudioPanel::audioClipEdited, this,
            [this](double gainDb, bool muted, qint64 fadeInMs, qint64 fadeOutMs, qint64 startMs) {
                int trackId = 0;
                const int id = m_selectedTrackClipId;
                if (id <= 0 || !m_timeline->trackClipById(id, &trackId))
                    return;
                m_timeline->setAudioClipLevel(id, gainDb, muted);
                m_timeline->setTrackClipFades(id, fadeInMs, fadeOutMs);
                m_timeline->moveTrackClip(id, trackId, startMs);
            });
    connect(m_audioPanel, &AudioPanel::trackLevelEdited, this, [this](double gainDb, bool muted) {
        int trackId = 0;
        if (m_selectedTrackClipId > 0 && m_timeline->trackClipById(m_selectedTrackClipId, &trackId))
            m_timeline->setTrackAudio(trackId, gainDb, muted);
    });
    connect(m_project, &Project::projectReset, this, [this] {
        selectTrackClip(-1);
        m_selectedClipId = 0; // ids don't survive a load / undo
    });

    // Trim from the transport buttons / [ ] shortcuts acts on the clip under
    // the playhead, at m_lastPositionMs (that clip's *source* time -- the
    // transport bar only knows timeline time). Timeline owns the clamp /
    // min-gap validation.
    connect(m_transport, &TransportBar::trimInRequested, this,
            [this] { trimAtPlayhead(true); });
    connect(m_transport, &TransportBar::trimOutRequested, this,
            [this] { trimAtPlayhead(false); });
    connect(m_transport, &TransportBar::trimCleared, this,
            [this] { clearTrim(); });

    // Any structural or trim change to the timeline -> rebuild the controller's
    // segments and repaint the widget, then keep the playhead on the same
    // source frame it was showing: its timeline position moves when its
    // own trim-in shifts, or when an earlier clip's length changes.
    connect(m_timeline, &Timeline::clipTrimChanged, this, [this](int, qint64, qint64) {
        remapPlayheadAfterEdit();
    });
    connect(m_timeline, &Timeline::clipInserted, this, [this](int) { remapPlayheadAfterEdit(); });
    connect(m_timeline, &Timeline::clipRemoved, this, [this](int) { remapPlayheadAfterEdit(); });
    connect(m_timeline, &Timeline::clipsReordered, this, [this] { remapPlayheadAfterEdit(); });
    connect(m_timeline, &Timeline::clipSpeedChanged, this,
            [this](int, double) { remapPlayheadAfterEdit(); });

    // Reframe authoring. The viewport is a dumb pose display + input device; this
    // window is the only thing that knows both the playhead and the Timeline.
    //  * arm toggle gates whether a look gesture writes a keyframe;
    //  * a finished drag / a zoom notch (lookCommitted) writes one at the
    //    playhead when armed;
    //  * during a drag we stop pushing the sampled pose so the path doesn't
    //    fight the user's hand.
    connect(m_transport, &TransportBar::reframeArmedChanged, this,
            [this](bool armed) { m_reframeArmed = armed; });
    connect(m_viewport, &EquirectViewportWindow::draggingChanged, this,
            [this](bool active) { m_viewportDragging = active; });
    connect(m_viewport, &EquirectViewportWindow::lookCommitted, this, [this] {
        if (m_reframeArmed && m_clipId)
            m_timeline->upsertClipReframeKey(
                m_clipId, m_lastPositionMs,
                {m_viewport->Yaw(), m_viewport->Pitch(), m_viewport->Roll(), m_viewport->FovY()});
        syncReframePanel(); // the drag / zoom moved the pose
    });

    // Reframe panel: a numeric edit sets the live viewport pose and, when
    // authoring is armed, keyframes it at the playhead -- same as a drag.
    connect(m_reframePanel, &ReframePanel::poseEdited, this, [this](const ReframePose &p) {
        m_viewport->SetPose(p.yaw, p.pitch, p.roll, p.fovY);
        if (m_reframeArmed && m_clipId)
            m_timeline->upsertClipReframeKey(m_clipId, m_lastPositionMs, p);
    });
    // Track subject: the panel arms a box select in the viewport; a drawn box
    // starts tracking from the playhead.
    m_reframePanel->setTrackUnavailable(syscheck::trackUnavailableReason());
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    connect(m_reframePanel, &ReframePanel::trackSubjectToggled, this,
            &MainWindow::setTrackSelecting);
    m_tracker = new SubjectTrackController(m_viewport, m_reframePanel->trackStopButton(), this);
    connect(m_reframePanel, &ReframePanel::trackStopRequested, m_tracker,
            &SubjectTrackController::stop);
    connect(m_tracker, &SubjectTrackController::progress, this, [this](qint64 ms, qint64 lostFor) {
        const SubjectTrackController::Run &run = m_tracker->run();
        const int idx = m_timeline->indexOfId(run.clipId);
        if (idx >= 0)
            m_timelineWidget->setPlayhead(
                m_timeline->clipToTimeline(idx, m_timeline->clipAt(idx).localAtSourceMs(ms)));
        const QString t = QString::number((ms - run.req.fromMs) / 1000.0, 'f', 1);
        m_reframePanel->setTrackStatus(
            lostFor < 0 ? tr("%1 s tracked. Following the subject.").arg(t)
                        : tr("%1 s. Lost the subject %2 s ago; stopping keeps the path up to "
                             "where it was last seen.")
                              .arg(t)
                              .arg(lostFor / 1000.0, 0, 'f', 1));
    });
    connect(m_tracker, &SubjectTrackController::finished, this, &MainWindow::finishSubjectTracking);
#endif
    // The viewport's box select serves Track subject and "Blur a plate…".
    connect(m_viewport, &EquirectViewportWindow::boxSelected, this, [this](const QRectF &box) {
        if (m_plateSelecting)
            startPlateFollow(box);
        else
            startSubjectTracking(box);
    });
    connect(m_viewport, &EquirectViewportWindow::boxSelectCanceled, this, [this] {
        if (m_plateSelecting)
            setPlateSelecting(false);
        else
            setTrackSelecting(false);
    });
    connect(m_transport, &TransportBar::deleteKeyRequested, this, [this] {
        if (m_clipId)
            m_timeline->removeClipReframeKeyNear(m_clipId, m_lastPositionMs);
    });
    connect(m_transport, &TransportBar::clearReframeRequested, this,
            [this] { if (m_clipId) m_timeline->clearClipReframe(m_clipId); });
    connect(m_timeline, &Timeline::clipReframeChanged, this, [this](int id) {
        refreshTimelineWidget(); // re-draw the keyframe diamonds
        if (id == m_clipId) {
            const ClipItem *c = m_timeline->clipById(id);
            if (c && !c->reframe.isEmpty() && !m_viewportDragging)
                applySampledPose(m_lastPositionMs); // reflect an edit/delete at once
            syncReframePanel();
        }
    });

    // Audio panel: an edit writes both level and mute onto the clip under the
    // playhead in one Timeline call. clipAudioChanged pushes the new
    // Segment list to AudioPlayer -- it's the only thing that reads it.
    connect(m_audioPanel, &AudioPanel::levelEdited, this, [this](double gainDb, bool muted) {
        if (const int id = editClipId())
            m_timeline->setClipAudio(id, gainDb, muted);
    });
    connect(m_timeline, &Timeline::mainAudioMutedChanged, this, [this] {
        pushTimelineToController(); // AudioPlayer reads the mute off the Segments
        refreshTimelineWidget();
    });
    connect(m_timeline, &Timeline::clipAudioChanged, this,
            [this](int id, double, bool) {
                pushTimelineToController(); // AudioPlayer needs the new gain/mute/track
                if (id == editClipId())
                    if (const ClipItem *c = m_timeline->clipById(id))
                        syncAudioPanel(*c);
            });
    // Noise reduction: onto the selected audio clip if the panel shows one
    // (the same rule as refreshAudioPanel), else the clip under the playhead.
    connect(m_audioPanel, &AudioPanel::noiseEdited, this, [this](const insv::NoiseReduction &n) {
        int trackId = 0;
        if (m_selectedTrackClipId > 0 &&
            m_timeline->trackClipById(m_selectedTrackClipId, &trackId) &&
            m_timeline->trackById(trackId)->kind == Track::Kind::Audio) {
            m_timeline->setAudioClipNoise(m_selectedTrackClipId, n);
            return;
        }
        if (const int id = editClipId())
            m_timeline->setClipNoise(id, n);
    });
    connect(m_audioPanel, &AudioPanel::trackEdited, this, [this](int track) {
        if (const int id = editClipId())
            m_timeline->setClipAudioTrack(id, track);
    });

    // Colour panel: an edit writes the grade onto the clip under the playhead;
    // clipColorChanged pushes it straight into the viewport if it's the
    // clip currently showing -- no re-stitch, applied at reprojection time.
    connect(m_colourPanel, &ColourPanel::gradeEdited, this, [this](const ColorGrade &g) {
        if (const int id = editClipId())
            m_timeline->setClipColor(id, g);
    });
    // Copy / paste a grade between clips: the panel's buttons act on its clip,
    // the timeline's right-click menu on the clicked one (or every clip --
    // one undo step, the edits land in one burst).
    const auto copyGrade = [this](int clipId) {
        const ClipItem *c = m_timeline->clipById(clipId);
        if (!c)
            return;
        m_copiedGrade = c->color;
        m_colourPanel->setCanPaste(true);
        m_timelineWidget->setCanPasteGrade(true);
    };
    const auto pasteGrade = [this](int clipId) {
        if (m_copiedGrade && clipId)
            m_timeline->setClipColor(clipId, *m_copiedGrade);
    };
    connect(m_colourPanel, &ColourPanel::copyRequested, this,
            [this, copyGrade] { copyGrade(editClipId()); });
    connect(m_colourPanel, &ColourPanel::pasteRequested, this,
            [this, pasteGrade] { pasteGrade(editClipId()); });
    connect(m_timelineWidget, &TimelineWidget::gradeCopyRequested, this, copyGrade);
    connect(m_timelineWidget, &TimelineWidget::gradePasteRequested, this, pasteGrade);
    connect(m_timelineWidget, &TimelineWidget::gradePasteAllRequested, this, [this] {
        if (!m_copiedGrade)
            return;
        for (int i = 0; i < m_timeline->count(); ++i)
            m_timeline->setClipColor(m_timeline->clipAt(i).id, *m_copiedGrade);
    });
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
        if (!c || c->sourcePath != m_stabilizerClipPath)
            return;
        ensureStabilizerFor(c->sourcePath);
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
        pushSeamAdjust();
        const ClipItem *c = m_timeline->clipById(id);
        const ClipItem *cur = m_timeline->clipById(m_clipId);
        if (c && cur && c->sourcePath == cur->sourcePath)
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs));
    });
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
            m_timelineWidget->setPlayhead(
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
    connect(m_timeline, &Timeline::clipColorChanged, this, [this](int id, ColorGrade grade) {
        if (id == m_clipId) // on screen
            applyColorGrade(grade);
        if (id == editClipId()) // in the panel
            m_colourPanel->setState(grade);
    });

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QAction *openProjectAction =
        fileMenu->addAction(tr("&Open project…"), this, [this] {
            if (!confirmDiscardChanges())
                return;
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Open project"), QDir::homePath(),
                tr("orbis-reframe project (*.%1)").arg(Project::kFileExtension));
            if (!path.isEmpty())
                openProject(path);
        });
    openProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    openProjectAction->setIcon(QIcon(QStringLiteral(":/icons/folder-open.svg")));
    QAction *addClipAction =
        fileMenu->addAction(tr("&Add clip to timeline…"), this, [this] { addClipToTimeline(); });
    addClipAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    addClipAction->setIcon(QIcon(QStringLiteral(":/icons/file-plus-2.svg")));
    QAction *addAudioAction = fileMenu->addAction(
        tr("Add a&udio file…"), this, [this] { addAudioFile(0, m_lastTimelineMs); });
    addAudioAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    QAction *addVideoAction = fileMenu->addAction(
        tr("Add &video overlay…"), this, [this] { addVideoFile(0, m_lastTimelineMs); });
    addVideoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    addVideoAction->setIcon(QIcon(QStringLiteral(":/icons/picture-in-picture-2.svg")));
    QAction *saveProjectAction =
        fileMenu->addAction(tr("&Save project"), this, &MainWindow::saveProject);
    saveProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    saveProjectAction->setIcon(QIcon(QStringLiteral(":/icons/save.svg")));
    QAction *saveProjectAsAction =
        fileMenu->addAction(tr("Save project &as…"), this, &MainWindow::saveProjectAs);
    saveProjectAsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    fileMenu->addSeparator();

    // File -> Export flat video… (disabled until a clip is open).
    m_exportAction = fileMenu->addAction(tr("&Export flat video…"), this,
                                        &MainWindow::exportFlatVideo);
    m_exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    m_exportAction->setIcon(QIcon(QStringLiteral(":/icons/download.svg")));
    m_exportAction->setEnabled(false);

    m_screenshotAction = fileMenu->addAction(tr("Save &screenshot"), this,
                                             &MainWindow::takeScreenshot);
    m_screenshotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    m_screenshotAction->setIcon(QIcon(QStringLiteral(":/icons/camera.svg")));
    m_screenshotAction->setEnabled(false);
    connect(m_transport, &TransportBar::screenshotRequested, this, &MainWindow::takeScreenshot);

    fileMenu->addSeparator();
    // close() runs closeEvent() -> confirmDiscardChanges(), so an unsaved
    // project still prompts before the app exits.
    QAction *quitAction = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    quitAction->setMenuRole(QAction::QuitRole);

    // Edit -> Undo/Redo. Project::undo()/redo() operate on whole-project
    // snapshots coalesced by a settle timer (see Project.cpp) -- there's no
    // per-action label, so these are just "Undo"/"Redo".
    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    m_undoAction = editMenu->addAction(tr("&Undo"), this, &MainWindow::undo);
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setEnabled(m_project->canUndo());
    m_redoAction = editMenu->addAction(tr("&Redo"), this, &MainWindow::redo);
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_redoAction->setEnabled(m_project->canRedo());
    connect(m_project, &Project::canUndoChanged, m_undoAction, &QAction::setEnabled);
    connect(m_project, &Project::canRedoChanged, m_redoAction, &QAction::setEnabled);

    setupKeyboardShortcuts();

    // Controller -> view.
    connect(m_controller, &PlaybackController::durationChanged, m_transport,
            &TransportBar::setDuration);
    connect(m_controller, &PlaybackController::openFailed, this,
            [this](const QString &error) {
                QMessageBox::warning(this, tr("Playback error"), error);
            });
    connect(m_controller, &PlaybackController::positionChanged, this,
            [this](qint64 timelineMs) {
                m_lastTimelineMs = timelineMs;
                m_transport->setPosition(timelineMs);
                m_timelineWidget->setPlayhead(timelineMs);
#ifdef ORBIS_REFRAME_HAVE_AUDIO
                m_audioPlayer->notePlaybackPosition(timelineMs);
#endif
                bindActiveClipAt(timelineMs);
                if (selectedOverlayClip())
                    refreshOverlayPanel(); // keyed values / key buttons follow the playhead
            });
    connect(m_controller, &PlaybackController::playingChanged,
            m_transport, &TransportBar::setPlaying);
    connect(m_controller, &PlaybackController::playingChanged, this, [this](bool playing) {
        m_playing = playing;
        if (playing) {
            // A measurement competes with playback for the GPU its whole run:
            // stop it; the pause starts it again.
            m_syncCancel = true;
            m_seamCancel = true;
            return;
        }
        if (m_measuredWhilePlaying) { // results held back while playing
            m_measuredWhilePlaying = false;
            pushSeamAdjust();
            if (const ClipItem *c = m_timeline->clipById(m_clipId))
                ensureStabilizerFor(c->sourcePath);
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs)); // re-stitch
        }
        scheduleClipScans(); // measurements held back while playing
    });
    connect(m_timeline, &Timeline::clipInserted, this, [this] { scheduleClipScans(); });
    connect(m_controller, &PlaybackController::bufferingChanged,
            m_transport, &TransportBar::setBuffering);
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    // Every play/pause entry point (transport button, Space, export pausing
    // playback, reaching end-of-timeline) funnels through this one signal.
    connect(m_controller, &PlaybackController::playingChanged, this, [this](bool playing) {
        if (playing)
            m_audioPlayer->play();
        else
            m_audioPlayer->pause();
    });
    // Hold the audio exactly while the video clock is frozen (a seek / stall)
    // so the two resume in step. Not bufferingChanged: that also fires
    // on ticks that merely find the queue empty while the clock runs on, and
    // holding the audio then chopped it for a second after every seek.
    connect(m_controller, &PlaybackController::clockHeldChanged, m_audioPlayer,
            &AudioPlayer::setHold);
#endif

    m_playbackThread->start();
}

MainWindow::~MainWindow()
{
    if (m_syncThread) {
        m_syncCancel = true;
        m_syncThread->wait();
    }
    if (m_seamThread) {
        m_seamCancel = true;
        m_seamThread->wait();
    }
    if (m_screenshotThread)
        m_screenshotThread->wait(); // one frame: a second or two at most
    // Background jobs that stitch and draw into the viewport: stopped and
    // waited for while the viewport is still there.
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    delete m_plateJobs;
    m_plateJobs = nullptr;
#endif
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    delete m_tracker;
    m_tracker = nullptr;
#endif
    m_playbackThread->quit();
    m_playbackThread->wait();
}

namespace {

// A widget that takes typed text, so it swallows the editor's single-key
// shortcuts while focused. Spin boxes report themselves as the focus widget,
// not their inner QLineEdit.
bool isTextEntry(const QWidget *w)
{
    return qobject_cast<const QAbstractSpinBox *>(w) || qobject_cast<const QLineEdit *>(w);
}

} // namespace

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    QWidget *fw = QApplication::focusWidget();
    if (!isTextEntry(fw))
        return QMainWindow::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        // Presses also arrive at the top-level QWindow before its widgets --
        // ignore those; the only bare QWindow that matters is the viewport.
        bool outside = false;
        if (watched == m_viewport) {
            outside = true;
        } else if (auto *w = qobject_cast<QWidget *>(watched)) {
            // Clicking the field itself (text, arrows) keeps it; clicking
            // another text field moves focus there on its own.
            bool inEntry = false;
            for (QWidget *p = w; p && !inEntry; p = p->parentWidget())
                inEntry = p == fw || isTextEntry(p);
            outside = !inEntry;
        }
        if (outside)
            fw->clearFocus();
        break;
    }
    case QEvent::ShortcutOverride:
        // A field keeps its own editing keys -- arrows, Home / End, Delete,
        // Backspace, Escape -- over the editor's unmodified shortcuts (a
        // spin box doesn't claim Up / Down itself). Ctrl / Alt ones still
        // reach the shortcuts.
        if (watched == fw) {
            const auto *ke = static_cast<QKeyEvent *>(event);
            if (!(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                event->accept();
                return true;
            }
        }
        break;
    case QEvent::KeyPress:
        // A field that handles Enter / Escape itself (the timeline's rename box).
        if (watched == fw && !fw->property("ownsReturnEscape").toBool()) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
                fw->clearFocus();
                return true;
            }
        }
        break;
    default:
        break;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (confirmDiscardChanges()) {
        // Let the viewport release its swapchain while it's still shown.
        // Closing destroys the window, which hides it first and only then
        // tells QVulkanWindow (SurfaceAboutToBeDestroyed); with Qt 6.8 on
        // Wayland (Debian 13) the NVIDIA driver crashes destroying a swapchain
        // whose window is already hidden. Releasing twice is harmless.
        QPlatformSurfaceEvent aboutToGo(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
        QCoreApplication::sendEvent(m_viewport, &aboutToGo);
        event->accept();
    } else
        event->ignore();
}

void MainWindow::applySampledPose(qint64 sourceMs)
{
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c)
        return;
    const ReframePose p = c->reframe.sample(sourceMs);
    m_viewport->SetPose(p.yaw, p.pitch, p.roll, p.fovY);
}

void MainWindow::ensureStabilizerFor(const QString &clipPath)
{
    // The stitch's capture-time correction (lens 1 lined up with lens 0)
    // rides on the stabilizer's gyro track, so it's (re)pushed whenever the
    // stabilizer is; off otherwise.
    auto rsOff = [this, &clipPath] {
        if (!clipPath.isEmpty())
            m_controller->setCaptureTiming(clipPath, {});
    };
    if (m_stabConfig.mode == insv::StabMode::Off) {
        rsOff();
        if (m_stabilizer.ready()) {
            m_stabilizer.build({}, insv::StabConfig{}); // clear
            const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            m_viewport->SetStabilization(ident);
        }
        m_stabilizerClipPath.clear();
        publishStabilizer();
        updateSyncStatus();
        return;
    }
    const double syncMs = std::isnan(syncAdjustFor(clipPath)) ? 0.0 : syncAdjustFor(clipPath);
    if (clipPath == m_stabilizerClipPath && m_stabilizerBuilt == m_stabConfig &&
        m_stabilizerSyncMs == syncMs)
        return;

    // Trailer read is a seek to the trailer + ~12 MB, not the whole file --
    // fast enough for a clip switch / settings change on the GUI thread.
    auto tele = insv::ReadInsvTelemetry(clipPath.toStdString());
    if (tele && tele->imu.size() >= 2) {
        // Derive the video<->IMU offset from the 0x400 exposure record
        // ("video t=0 == IMU sample 0" is off by ~tens of ms, varying per clip).
        double fps = 0.0;
        for (int i = 0; i < m_timeline->count(); ++i)
            if (m_timeline->clipAt(i).sourcePath == clipPath) {
                fps = m_timeline->clipAt(i).fps;
                break;
            }
        m_stabilizer.build(*tele, fps, m_stabConfig, syncMs / 1000.0);
        m_stabilizerClipPath = clipPath;
        m_stabilizerBuilt = m_stabConfig;
        m_stabilizerSyncMs = syncMs;
        m_controller->setCaptureTiming(
            clipPath, ReframeExportController::captureTimingFor(
                          std::make_shared<const insv::GyroStabilizer>(m_stabilizer)));
    } else {
        m_stabilizer.build({}, insv::StabConfig{});
        m_stabilizerClipPath.clear();
        rsOff();
    }
    publishStabilizer();
    applyStabilization(m_lastPositionMs); // the frame on screen, if paused
    updateSyncStatus();
}

double MainWindow::syncAdjustFor(const QString &clipPath) const
{
    for (int i = 0; i < m_timeline->count(); ++i)
        if (m_timeline->clipAt(i).sourcePath == clipPath)
            return m_timeline->clipAt(i).syncAdjustMs;
    return std::nan("");
}

void MainWindow::maybeMeasureSync(const QString &clipPath)
{
#ifdef ORBIS_REFRAME_HAVE_CLIP_SYNC
    if (clipPath.isEmpty() || m_syncThread || !std::isnan(syncAdjustFor(clipPath)) ||
        m_syncFailed.contains(clipPath))
        return;
    const ClipItem *clip = nullptr;
    for (int i = 0; i < m_timeline->count() && !clip; ++i)
        if (m_timeline->clipAt(i).sourcePath == clipPath)
            clip = &m_timeline->clipAt(i);
    if (!clip || clip->mediaMissing)
        return;
    const std::string path = clipPath.toStdString();
    const qint64 in = clip->trimInMs, out = clip->trimOutMs;
    const double fps = clip->fps;
    m_syncMeasuringPath = clipPath;
    m_syncCancel = false;
    const auto guard = m_project->previewStitch().lensGuard;
    m_syncThread = QThread::create([this, path, in, out, fps, clipPath, guard] {
        const stitch::ClipSyncResult r =
            stitch::MeasureClipSync(path, in, out, fps, &m_syncCancel, guard);
        QMetaObject::invokeMethod(this, [this, clipPath, r] {
            m_syncThread->wait(); // its function has just returned
            m_syncThread->deleteLater();
            m_syncThread = nullptr;
            m_syncMeasuringPath.clear();
            m_syncLastMessage = QString::fromStdString(r.message);
            core::log::Info("[sync] %s: %s\n", qPrintable(clipPath), r.message.c_str());
            if (r.ok)
                m_timeline->setSourceSyncAdjust(clipPath, r.adjustMs); // -> clipSyncChanged
            else if (!m_syncCancel)
                m_syncFailed.insert(clipPath);
            updateSyncStatus();
            scheduleClipScans(); // another source may be waiting for its turn
        }, Qt::QueuedConnection);
    });
    m_syncThread->start(QThread::LowPriority);
    core::log::Info("[sync] %s: measuring\n", qPrintable(clipPath));
    updateSyncStatus();
#else
    Q_UNUSED(clipPath);
#endif
}

bool MainWindow::seamAdjustFor(const QString &clipPath, std::array<double, 3> &deg) const
{
    const int guard = static_cast<int>(m_project->previewStitch().lensGuard);
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        if (c.sourcePath == clipPath && c.seamAdjustGuard == guard) {
            deg = c.seamAdjustDeg;
            return true;
        }
    }
    deg = {0.0, 0.0, 0.0};
    return false;
}

void MainWindow::pushSeamAdjust()
{
    for (int i = 0; i < m_timeline->count(); ++i) {
        const QString &path = m_timeline->clipAt(i).sourcePath;
        std::array<double, 3> deg;
        seamAdjustFor(path, deg);
        m_controller->setSeamAdjust(path, deg);
    }
}

void MainWindow::maybeMeasureSeam(const QString &clipPath)
{
    std::array<double, 3> deg;
    if (clipPath.isEmpty() || m_seamThread || seamAdjustFor(clipPath, deg) ||
        m_seamFailed.contains(clipPath))
        return;
    const ClipItem *clip = nullptr;
    for (int i = 0; i < m_timeline->count() && !clip; ++i)
        if (m_timeline->clipAt(i).sourcePath == clipPath)
            clip = &m_timeline->clipAt(i);
    if (!clip || clip->mediaMissing)
        return;
    const std::string path = clipPath.toStdString();
    const qint64 in = clip->trimInMs, out = clip->trimOutMs;
    const auto guard = m_project->previewStitch().lensGuard;
    m_seamMeasuringPath = clipPath;
    m_seamCancel = false;
    m_seamThread = QThread::create([this, path, in, out, clipPath, guard] {
        const insv::SeamAlignmentResult r = stitch::MeasureClipSeam(path, in, out, guard, &m_seamCancel);
        QMetaObject::invokeMethod(this, [this, clipPath, r, guard] {
            m_seamThread->wait(); // its function has just returned
            m_seamThread->deleteLater();
            m_seamThread = nullptr;
            m_seamMeasuringPath.clear();
            m_seamLastMessage = QString::fromStdString(r.message);
            core::log::Info("[seam] %s: %s\n", qPrintable(clipPath), r.message.c_str());
            // Stale if the guard setting changed meanwhile: drop it and let the
            // re-measure below run under the new one.
            if (r.ok && guard == m_project->previewStitch().lensGuard)
                m_timeline->setSourceSeamAdjust(
                    clipPath, {r.adjustDeg[0], r.adjustDeg[1], r.adjustDeg[2]},
                    static_cast<int>(guard)); // -> clipSeamChanged
            else if (!r.ok && !m_seamCancel)
                m_seamFailed.insert(clipPath);
            updateSeamStatus();
            scheduleClipScans(); // another source may be waiting for its turn
        }, Qt::QueuedConnection);
    });
    m_seamThread->start(QThread::LowPriority);
    core::log::Info("[seam] %s: measuring\n", qPrintable(clipPath));
}

void MainWindow::scheduleClipScans()
{
    if (m_scanQueued)
        return;
    m_scanQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_scanQueued = false;
        if (m_playing)
            return; // picked up again on pause
        for (int i = 0; i < m_timeline->count(); ++i) {
            const QString path = m_timeline->clipAt(i).sourcePath;
            maybeMeasureSync(path);
            maybeMeasureSeam(path);
        }
        updateSyncStatus();
        updateSeamStatus();
    }, Qt::QueuedConnection);
}

void MainWindow::updateSeamStatus()
{
    const ClipItem *c = m_timeline->clipById(m_clipId);
    QString text;
    std::array<double, 3> deg;
    if (c && seamAdjustFor(c->sourcePath, deg))
        text = tr("Seam: aligned for this clip (back lens yaw %1°, pitch %2°, roll %3°).")
                   .arg(deg[0], 0, 'f', 2)
                   .arg(deg[1], 0, 'f', 2)
                   .arg(deg[2], 0, 'f', 2);
    else if (c && m_seamMeasuringPath == c->sourcePath)
        text = tr("Seam: measuring this clip…");
    else if (c && m_seamFailed.contains(c->sourcePath))
        text = tr("Seam: couldn't align this clip (%1).").arg(m_seamLastMessage);
    m_stitchingPanel->setSeamStatus(text);
}

void MainWindow::updateSyncStatus()
{
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (m_stabConfig.mode == insv::StabMode::Off || !c) {
        m_stabilizationPanel->setSyncStatus(QString());
        return;
    }
    const double ms = syncAdjustFor(c->sourcePath);
    QString text;
    if (!std::isnan(ms))
        text = tr("Gyro sync: fine-tuned for this clip (%1 ms).").arg(ms, 0, 'f', 1);
    else if (m_syncMeasuringPath == c->sourcePath)
        text = tr("Gyro sync: measuring this clip…");
    else if (m_syncFailed.contains(c->sourcePath))
        text = tr("Gyro sync: couldn't fine-tune this clip (%1); using the camera's timing.")
                   .arg(m_syncLastMessage);
#ifndef ORBIS_REFRAME_HAVE_CLIP_SYNC
    else
        text = tr("Gyro sync: camera timing (fine-tune needs a build with OpenCV).");
#endif
    m_stabilizationPanel->setSyncStatus(text);
}

void MainWindow::publishStabilizer()
{
    auto snap = m_stabilizer.ready() ? std::make_shared<const insv::GyroStabilizer>(m_stabilizer)
                                     : nullptr;
    std::lock_guard<std::mutex> lk(m_stabShared->mx);
    m_stabShared->off = m_stabConfig.mode == insv::StabMode::Off || !snap;
    m_stabShared->path = m_stabilizerClipPath;
    m_stabShared->stab = std::move(snap);
}

void MainWindow::applyStabilization(qint64 sourceMs)
{
    float m[3][3];
    m_stabilizer.correctionMatrix(sourceMs / 1000.0, m); // identity when not ready
    m_viewport->SetStabilization(&m[0][0]);
}

void MainWindow::syncReframePanel()
{
    m_reframePanel->setPose({m_viewport->Yaw(), m_viewport->Pitch(), m_viewport->Roll(),
                             m_viewport->FovY()});
}

void MainWindow::applyColorGrade(const ColorGrade &grade)
{
    m_viewport->SetColorGrade(grade.packed().data());
}

void MainWindow::bindActiveClipAt(qint64 timelineMs)
{
    const Timeline::Point pt = m_timeline->timelineToClip(timelineMs);
    if (pt.clipIndex < 0) {
        m_clipId = 0;
        m_lastPositionMs = 0;
        return;
    }
    const ClipItem &c = m_timeline->clipAt(pt.clipIndex);
    const bool clipChanged = c.id != m_clipId;
    m_clipId = c.id;
    if (clipChanged) {
        pushSeamAdjust();
        updateSeamStatus();
    }
    m_timelineWidget->setActiveClip(editClipId());
    m_lastPositionMs = c.sourceAtLocalMs(pt.clipLocalMs); // this clip's source time
    // The stab matrix itself rides with each presented frame (the playback
    // controller's stabilization provider) -- setting it here from the clock
    // position would put it up to a frame out of step with the picture.
    ensureStabilizerFor(c.sourcePath);
    if (!c.reframe.isEmpty() && !m_viewportDragging)
        applySampledPose(m_lastPositionMs);
    if (!m_viewportDragging)
        syncReframePanel();
    if (clipChanged) {
        applyColorGrade(c.color); // the viewport shows this clip
        if (!selectedVideoClip()) // the panels follow the playhead unless a clip is selected
            refreshClipPanels();
    }
}

const ClipItem *MainWindow::selectedVideoClip() const
{
    return m_selectedClipId > 0 ? m_timeline->clipById(m_selectedClipId) : nullptr;
}

int MainWindow::editClipId() const
{
    if (const ClipItem *c = selectedVideoClip())
        return c->id;
    return m_clipId;
}

void MainWindow::selectVideoClip(int clipId)
{
    if (clipId > 0 && !m_timeline->clipById(clipId))
        clipId = 0;
    m_selectedClipId = clipId;
    if (clipId > 0 && m_selectedTrackClipId > 0)
        selectTrackClip(-1); // one selection at a time
    refreshClipPanels();
}

void MainWindow::refreshClipPanels()
{
    m_timelineWidget->setActiveClip(editClipId());
    const ClipItem *c = m_timeline->clipById(editClipId());
    if (!c) {
        m_privacyPanel->setNoClip();
        refreshAudioPanel();
        return;
    }
    syncMediaInfoPanel(*c);
    m_colourPanel->setState(c->color);
    syncPrivacyPanel();
    refreshAudioPanel(); // the selected audio clip, if any, else this clip
    const ClipItem *playing = m_timeline->clipById(m_clipId);
    m_transport->setHasReframeKeys(playing && !playing->reframe.isEmpty());
    m_reframePanel->setClipUnkeyed(playing && playing->reframe.isEmpty());
}

TimelineWidget::Peaks MainWindow::waveformFor(const QString &path, int stream)
{
    const WaveformCache::Peaks p = m_waveforms->peaks(path, stream);
    return p ? TimelineWidget::Peaks(p, &p->peaks) : nullptr; // shares ownership with p
}

int MainWindow::audioStreamCount(const QString &path)
{
    auto it = m_audioTrackCounts.constFind(path);
    if (it == m_audioTrackCounts.constEnd())
        it = m_audioTrackCounts.insert(
            path, insv::InsvAudioDemux::countAudioStreams(path.toStdString()));
    return *it;
}

void MainWindow::syncAudioPanel(const ClipItem &c)
{
    int selTrack = 0;
    if (m_selectedTrackClipId > 0 && m_timeline->trackClipById(m_selectedTrackClipId, &selTrack) &&
        m_timeline->trackById(selTrack)->kind == Track::Kind::Audio)
        return; // the panel shows the selected audio clip instead
    // Streams of this clip's footage that are also on an audio track, so
    // the picker can't play one twice.
    QHash<int, QString> detached;
    for (int i = m_timeline->videoTrackCount(); i < m_timeline->trackCount(); ++i) {
        const Track &t = m_timeline->trackAt(i);
        for (const TrackClip &a : t.clips)
            if (a.stream >= 0 && a.sourcePath == c.sourcePath && a.trimInMs < c.trimOutMs &&
                c.trimInMs < a.trimOutMs)
                detached.insert(a.stream, t.name);
    }
    m_audioPanel->setState(c.audioGainDb, c.audioMuted, c.audioTrack,
                           audioStreamCount(c.sourcePath), QFileInfo(c.sourcePath).fileName(),
                           detached, c.noise);
}

void MainWindow::refreshTrackRows()
{
    QVector<TimelineWidget::TrackView> tracks;
    for (int i = 0; i < m_timeline->trackCount(); ++i) {
        const Track &t = m_timeline->trackAt(i);
        TimelineWidget::TrackView v;
        v.id = t.id;
        v.name = t.name;
        v.muted = t.muted;
        v.video = t.kind == Track::Kind::Video;
        for (const TrackClip &c : t.clips) {
            TimelineWidget::TrackClipView a;
            a.id = c.id;
            a.label = QFileInfo(c.sourcePath).fileName();
            if (v.video) { // an overlay: no audio to draw, nothing to link
                a.startMs = c.startMs;
                a.trimInMs = c.trimInMs;
                a.trimOutMs = c.trimOutMs;
                a.sourceDurationMs = c.sourceDurationMs;
                a.speed = c.speed;
                a.fadeInMs = c.fadeInMs;
                a.fadeOutMs = c.fadeOutMs;
                a.missing = c.mediaMissing;
                for (const InsetKey &k : c.insetKeys.keys())
                    a.keySourceMs.push_back(k.timeMs);
                a.hasSound = !c.mediaMissing && audioStreamCount(c.sourcePath) > 0;
                v.clips.push_back(a);
                continue;
            }
            if (c.stream >= 0) // a detached camera stream
                a.label += tr(" (stream %1)").arg(c.stream + 1);
            a.startMs = c.startMs;
            a.trimInMs = c.trimInMs;
            a.trimOutMs = c.trimOutMs;
            a.sourceDurationMs = c.sourceDurationMs;
            a.speed = c.speed;
            a.fadeInMs = c.fadeInMs;
            a.fadeOutMs = c.fadeOutMs;
            a.muted = c.muted;
            a.missing = c.mediaMissing;
            a.linked = c.linkedClipId != 0;
            if (!c.mediaMissing)
                a.peaks = waveformFor(c.sourcePath, c.stream);
            a.gain = std::pow(10.0, c.gainDb / 20.0);
            a.linkable = !a.linked && linkTargetFor(c) != 0;
            v.clips.push_back(a);
        }
        tracks.push_back(std::move(v));
    }
    m_timelineWidget->setTracks(tracks);
    m_timelineWidget->setSelectedTrackClip(m_selectedTrackClipId);
}

void MainWindow::selectTrackClip(int clipId)
{
    if (clipId > 0 && !m_timeline->trackClipById(clipId))
        clipId = -1;
    const bool changed = clipId != m_selectedTrackClipId;
    m_selectedTrackClipId = clipId;
    m_timelineWidget->setSelectedTrackClip(clipId);
    if (clipId > 0 && m_selectedClipId > 0) { // one selection at a time
        m_selectedClipId = 0;
        refreshClipPanels();
    } else if (changed) {
        refreshAudioPanel();
    }
    refreshOverlayPanel();
    if (changed && selectedOverlayClip())
        m_sidebar->raisePanel(m_overlayPanel);
}

void MainWindow::refreshAudioPanel()
{
    int trackId = 0;
    const TrackClip *a = m_selectedTrackClipId > 0
                             ? m_timeline->trackClipById(m_selectedTrackClipId, &trackId)
                             : nullptr;
    if (a && m_timeline->trackById(trackId)->kind == Track::Kind::Video)
        a = nullptr; // an overlay: the Overlay panel's; this one follows the playhead
    if (a) {
        const Track *t = m_timeline->trackById(trackId);
        AudioPanel::AudioClipState st;
        st.label = QFileInfo(a->sourcePath).fileName();
        if (a->stream >= 0)
            st.label += tr(" (stream %1)").arg(a->stream + 1);
        st.gainDb = a->gainDb;
        st.muted = a->muted;
        st.fadeInMs = a->fadeInMs;
        st.fadeOutMs = a->fadeOutMs;
        st.spanMs = a->timelineDurationMs();
        st.startMs = a->startMs;
        st.linked = a->linkedClipId != 0;
        st.trackName = t->name;
        st.trackGainDb = t->gainDb;
        st.trackMuted = t->muted;
        st.noise = a->noise;
        m_audioPanel->setAudioClipState(st);
        return;
    }
    if (const ClipItem *c = m_timeline->clipById(editClipId()))
        syncAudioPanel(*c);
    else
        m_audioPanel->setNoClip();
}

std::vector<insv::MixClip> MainWindow::mixClips() const
{
    constexpr int kFramesPerMs = 48; // playback and export both mix at 48 kHz
    std::vector<insv::MixClip> mix;
    for (int i = 0; i < m_timeline->trackCount(); ++i) {
        const Track &t = m_timeline->trackAt(i);
        if (t.muted || t.kind != Track::Kind::Audio)
            continue;
        for (const TrackClip &c : t.clips) {
            if (c.muted || c.mediaMissing)
                continue;
            insv::MixClip m;
            m.path = c.sourcePath.toStdString();
            m.stream = c.stream;
            m.startFrame = c.startMs * kFramesPerMs;
            m.lengthFrames = c.timelineDurationMs() * kFramesPerMs;
            m.inFrame = c.trimInMs * kFramesPerMs;
            m.speed = c.speed;
            m.gain = static_cast<float>(std::pow(10.0, (c.gainDb + t.gainDb) / 20.0));
            m.fadeInFrames = c.fadeInMs * kFramesPerMs;
            m.fadeOutFrames = c.fadeOutMs * kFramesPerMs;
            m.noise = c.noise;
            mix.push_back(std::move(m));
        }
    }
    return mix;
}

void MainWindow::onTracksChanged()
{
    if (m_selectedTrackClipId > 0 && !m_timeline->trackClipById(m_selectedTrackClipId))
        m_selectedTrackClipId = -1;
    refreshTrackRows();
    refreshAudioPanel();
    refreshOverlayPanel();
    pushInsetsToController();
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->setMix(mixClips());
#endif
}

void MainWindow::addAudioFile(int trackId, qint64 atMs)
{
    if (m_timeline->count() == 0)
        return; // the main track sets the timeline's length: nothing to lay audio under
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Add audio files"),
        m_lastAudioDir.isEmpty() ? QDir::homePath() : m_lastAudioDir,
        tr("Audio files (*.mp3 *.wav *.flac *.aac *.m4a *.ogg *.opus);;All files (*)"));
    if (paths.isEmpty())
        return;
    m_lastAudioDir = QFileInfo(paths.first()).absolutePath();
    addAudioFiles(paths, trackId, atMs);
}

void MainWindow::addAudioFiles(const QStringList &paths, int trackId, qint64 atMs)
{
    if (m_timeline->count() == 0)
        return;
    // Probe them all first, so the track picked has room for the lot.
    QList<std::pair<QString, qint64>> ok;
    QStringList failed;
    qint64 total = 0;
    for (const QString &path : paths) {
        insv::InsvAudioDemux probe;
        std::string err;
        if (!probe.open(path.toStdString(), 48000, 2, err) || probe.durationMs() <= 0) {
            failed << QFileInfo(path).fileName() +
                          (err.empty() ? QString() : QStringLiteral(": ") + QString::fromStdString(err));
            continue;
        }
        ok.append({path, probe.durationMs()});
        total += probe.durationMs();
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Could not add audio"),
                             tr("No audio found in:\n%1").arg(failed.join(QLatin1Char('\n'))));
    if (ok.isEmpty())
        return;
    atMs = std::clamp<qint64>(atMs, 0, m_timeline->totalDurationMs());

    // No track given: the first audio track with room for them there, else a
    // new one. Several files go one after another.
    if (trackId <= 0 || !m_timeline->trackById(trackId) ||
        m_timeline->trackById(trackId)->kind != Track::Kind::Audio) {
        trackId = freeTrackFor(atMs, atMs + total, false, [](const Track &) { return true; });
        if (!trackId)
            trackId = m_timeline->addTrack(Track::Kind::Audio);
    }
    int last = 0;
    for (const auto &[path, durMs] : ok) {
        last = m_timeline->addAudioClip(trackId, path, durMs, atMs);
        atMs += durMs;
    }
    selectTrackClip(last);
}

void MainWindow::addVideoFile(int trackId, qint64 atMs)
{
    if (m_timeline->count() == 0)
        return; // the main track sets the timeline's length: nothing to lay an overlay over
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Add video overlay"),
        m_lastVideoDir.isEmpty() ? QDir::homePath() : m_lastVideoDir,
        tr("Video files (*.mp4 *.mov *.mkv *.m4v *.webm *.avi);;All files (*)"));
    if (paths.isEmpty())
        return;
    m_lastVideoDir = QFileInfo(paths.first()).absolutePath();
    addVideoFiles(paths, trackId, atMs);
}

void MainWindow::addVideoFiles(const QStringList &paths, int trackId, qint64 atMs)
{
    if (m_timeline->count() == 0)
        return;
    struct Probed {
        QString path;
        exportpipe::FlatVideoReader::Info info;
    };
    QList<Probed> ok;
    QStringList failed;
    qint64 total = 0;
    for (const QString &path : paths) {
        Probed p{path, {}};
        std::string err;
        if (!exportpipe::FlatVideoReader::probe(path.toStdString(), p.info, err)) {
            failed << QFileInfo(path).fileName() +
                          (err.empty() ? QString() : QStringLiteral(": ") + QString::fromStdString(err));
            continue;
        }
        ok.append(p);
        total += p.info.durationMs;
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Could not add video"),
                             tr("No video found in:\n%1").arg(failed.join(QLatin1Char('\n'))));
    if (ok.isEmpty())
        return;
    atMs = std::clamp<qint64>(atMs, 0, m_timeline->totalDurationMs());

    // No overlay track given: the first with room for them there, else a new
    // one. Several files go one after another.
    const Track *given = trackId > 0 ? m_timeline->trackById(trackId) : nullptr;
    if (!given || given->kind != Track::Kind::Video) {
        trackId = freeTrackFor(atMs, atMs + total, true, [](const Track &) { return true; });
        if (!trackId)
            trackId = m_timeline->addTrack(Track::Kind::Video);
    }
    int last = 0;
    for (const Probed &p : ok) {
        last = m_timeline->addVideoClip(trackId, p.path, p.info.durationMs, atMs, p.info.width,
                                        p.info.height);
        atMs += p.info.durationMs;
    }
    selectTrackClip(last);
}

void MainWindow::pushInsetsToController()
{
    std::vector<InsetFeed::Clip> clips;
    // Bottom-most first: the lowest overlay row draws first, the top row last.
    for (int i = m_timeline->videoTrackCount() - 1; i >= 0; --i) {
        const Track &t = m_timeline->trackAt(i);
        if (t.muted)
            continue; // hidden
        for (const TrackClip &c : t.clips) {
            if (c.mediaMissing)
                continue;
            InsetFeed::Clip f;
            f.id = c.id;
            f.path = c.sourcePath;
            f.startMs = c.startMs;
            f.trimInMs = c.trimInMs;
            f.trimOutMs = c.trimOutMs;
            f.speed = c.speed;
            f.aspect = c.aspect();
            f.fadeInMs = c.fadeInMs;
            f.fadeOutMs = c.fadeOutMs;
            f.look = c.look;
            f.frame = c.frame;
            f.keys = c.insetKeys;
            clips.push_back(std::move(f));
        }
    }
    m_controller->setInsets(std::move(clips)); // thread-safe
}

std::vector<exportpipe::InsetClipJob> MainWindow::insetJobs() const
{
    std::vector<exportpipe::InsetClipJob> jobs;
    for (int i = m_timeline->videoTrackCount() - 1; i >= 0; --i) {
        const Track &t = m_timeline->trackAt(i);
        if (t.muted)
            continue;
        for (const TrackClip &c : t.clips) {
            if (c.mediaMissing)
                continue;
            exportpipe::InsetClipJob j;
            j.path = c.sourcePath.toStdString();
            j.startMs = c.startMs;
            j.trimInMs = c.trimInMs;
            j.trimOutMs = c.trimOutMs;
            j.speed = c.speed;
            j.aspect = c.aspect();
            j.fadeInMs = c.fadeInMs;
            j.fadeOutMs = c.fadeOutMs;
            j.frame = c.frame;
            // A copy: the export thread samples it.
            j.lookAt = [look = c.look, keys = c.insetKeys](int64_t sourceMs) {
                return keys.isEmpty() ? look : keys.sample(sourceMs);
            };
            jobs.push_back(std::move(j));
        }
    }
    return jobs;
}

const TrackClip *MainWindow::selectedOverlayClip(int *trackId) const
{
    int tid = 0;
    const TrackClip *c =
        m_selectedTrackClipId > 0 ? m_timeline->trackClipById(m_selectedTrackClipId, &tid) : nullptr;
    if (!c || m_timeline->trackById(tid)->kind != Track::Kind::Video)
        return nullptr;
    if (trackId)
        *trackId = tid;
    return c;
}

void MainWindow::refreshOverlayPanel()
{
    int trackId = 0;
    const TrackClip *c = selectedOverlayClip(&trackId);
    if (!c) {
        m_overlayPanel->setNoClip();
        return;
    }
    OverlayPanel::State st;
    st.label = QFileInfo(c->sourcePath).fileName();
    st.trackName = m_timeline->trackById(trackId)->name;
    st.playheadInClip = m_lastTimelineMs >= c->startMs && m_lastTimelineMs < c->endMs();
    const qint64 src = c->sourceAtLocalMs(
        std::clamp<qint64>(m_lastTimelineMs - c->startMs, 0, c->timelineDurationMs()));
    st.look = c->lookAt(src);
    st.frame = c->frame;
    st.fadeInMs = c->fadeInMs;
    st.fadeOutMs = c->fadeOutMs;
    st.spanMs = c->timelineDurationMs();
    st.startMs = c->startMs;
    st.keyCount = c->insetKeys.count();
    for (const InsetKey &k : c->insetKeys.keys())
        st.keyAtPlayhead |= st.playheadInClip &&
                            std::llabs(k.timeMs - src) <= Timeline::kInsetKeyDeleteSnapMs;
    m_overlayPanel->setState(st);
}

void MainWindow::setOverlayKeyAtPlayhead()
{
    const TrackClip *c = selectedOverlayClip();
    if (!c)
        return;
    if (m_lastTimelineMs < c->startMs || m_lastTimelineMs >= c->endMs()) {
        m_transport->showNotice(tr("Move the playhead into the overlay clip to set a key"), false);
        return;
    }
    const qint64 src = c->sourceAtLocalMs(m_lastTimelineMs - c->startMs);
    m_timeline->upsertTrackClipInsetKey(c->id, src, c->lookAt(src));
}

void MainWindow::deleteOverlayKeyAtPlayhead()
{
    if (const TrackClip *c = selectedOverlayClip())
        m_timeline->removeTrackClipInsetKeyNear(
            c->id, c->sourceAtLocalMs(m_lastTimelineMs - c->startMs));
}

void MainWindow::clearOverlayKeys()
{
    if (const TrackClip *c = selectedOverlayClip())
        m_timeline->clearTrackClipInsetKeys(
            c->id, c->sourceAtLocalMs(std::clamp<qint64>(m_lastTimelineMs - c->startMs, 0,
                                                         c->timelineDurationMs())));
}

void MainWindow::applyInsetLook(int clipId, const inset::Look &look)
{
    const TrackClip *c = m_timeline->trackClipById(clipId);
    if (!c)
        return;
    if (c->insetKeys.isEmpty()) {
        m_timeline->setTrackClipInset(clipId, look, c->frame);
        return;
    }
    // Keyed: the edit becomes a key at the playhead, which has to be over it.
    if (m_lastTimelineMs < c->startMs || m_lastTimelineMs >= c->endMs()) {
        m_transport->showNotice(tr("Move the playhead into the overlay clip to set a key"), false);
        refreshOverlayPanel(); // put the fields back
        pushInsetsToController();
        return;
    }
    m_timeline->upsertTrackClipInsetKey(clipId, c->sourceAtLocalMs(m_lastTimelineMs - c->startMs),
                                        look);
}

int MainWindow::freeTrackFor(qint64 startMs, qint64 endMs, bool video,
                             const std::function<bool(const Track &)> &accept) const
{
    for (int i = 0; i < m_timeline->trackCount(); ++i) {
        const Track &t = m_timeline->trackAt(i);
        if ((t.kind == Track::Kind::Video) != video)
            continue;
        const bool free = std::none_of(t.clips.begin(), t.clips.end(), [&](const TrackClip &c) {
            return c.startMs < endMs && startMs < c.endMs();
        });
        if (free && accept(t))
            return t.id;
    }
    return 0;
}

int MainWindow::linkTargetFor(const TrackClip &a) const
{
    // A main-track or overlay clip of the same footage whose source range
    // overlaps the audio's; the one nearest on the timeline wins (the audio
    // was detached from it, or cut from it).
    int best = 0;
    qint64 bestDist = std::numeric_limits<qint64>::max();
    auto consider = [&](int id, qint64 startMs, qint64 trimInMs, qint64 trimOutMs, double speed) {
        if (a.trimOutMs <= trimInMs || trimOutMs <= a.trimInMs)
            return;
        const qint64 where = startMs + std::llround((a.trimInMs - trimInMs) / speed);
        const qint64 dist = std::abs(where - a.startMs);
        if (dist < bestDist) {
            bestDist = dist;
            best = id;
        }
    };
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        if (c.sourcePath == a.sourcePath)
            consider(c.id, m_timeline->clipToTimeline(i, 0), c.trimInMs, c.trimOutMs, c.speed);
    }
    for (int i = 0; i < m_timeline->videoTrackCount(); ++i)
        for (const TrackClip &c : m_timeline->trackAt(i).clips)
            if (c.sourcePath == a.sourcePath)
                consider(c.id, c.startMs, c.trimInMs, c.trimOutMs, c.speed);
    return best;
}

void MainWindow::trimAtPlayhead(bool in)
{
    // A selected audio clip takes the trim, cut at the playhead's point
    // in it; the playhead has to be over the clip.
    int trackId = 0;
    if (const TrackClip *a = m_selectedTrackClipId > 0
                                 ? m_timeline->trackClipById(m_selectedTrackClipId, &trackId)
                                 : nullptr) {
        if (m_lastTimelineMs <= a->startMs || m_lastTimelineMs >= a->endMs()) {
            m_transport->showNotice(tr("Move the playhead into the selected clip to trim it"),
                                    false);
            return;
        }
        const qint64 src = a->sourceAtLocalMs(m_lastTimelineMs - a->startMs);
        if (in)
            m_timeline->setTrackClipTrimIn(a->id, src);
        else
            m_timeline->setTrackClipTrimOut(a->id, src);
        return;
    }
    // A selected video clip only if the playhead is over it (the playhead's
    // source time, m_lastPositionMs, is m_clipId's).
    if (!m_clipId)
        return;
    if (editClipId() != m_clipId) {
        m_transport->showNotice(tr("Move the playhead into the selected clip to trim it"), false);
        return;
    }
    if (in)
        m_timeline->setClipTrimIn(m_clipId, m_lastPositionMs);
    else
        m_timeline->setClipTrimOut(m_clipId, m_lastPositionMs);
}

void MainWindow::addKey(const QString &group, const QString &what,
                        const QList<QKeySequence> &keys, const std::function<void()> &fn,
                        const QString &keysLabel)
{
    for (const QKeySequence &seq : keys) {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::ApplicationShortcut);
        sc->setAutoRepeat(true);
        connect(sc, &QShortcut::activated, this, fn);
    }
    m_keyHelp.append({group, what, keys, keysLabel});
}

void MainWindow::setupKeyboardShortcuts()
{
    const QString play = tr("Playback");
    addKey(play, tr("Play / pause"), {QKeySequence(Qt::Key_Space)},
           [this] { QMetaObject::invokeMethod(m_controller, "togglePlay", Qt::QueuedConnection); });
    addKey(play, tr("Previous / next frame (pauses)"),
           {QKeySequence(Qt::Key_Left)},
           [this] { navigateTo(m_timeline->stepFrames(m_lastTimelineMs, -1), true); });
    addKey(play, QString(), {QKeySequence(Qt::Key_Right)},
           [this] { navigateTo(m_timeline->stepFrames(m_lastTimelineMs, 1), true); });
    addKey(play, tr("Back / forward 1 second"), {QKeySequence(Qt::SHIFT | Qt::Key_Left)},
           [this] { navigateTo(m_lastTimelineMs - 1000, false); });
    addKey(play, QString(), {QKeySequence(Qt::SHIFT | Qt::Key_Right)},
           [this] { navigateTo(m_lastTimelineMs + 1000, false); });
    addKey(play, tr("Previous / next cut or clip edge"), {QKeySequence(Qt::Key_Up)},
           [this] { navigateTo(m_timeline->adjacentEditPoint(m_lastTimelineMs, -1), false); });
    addKey(play, QString(), {QKeySequence(Qt::Key_Down)},
           [this] { navigateTo(m_timeline->adjacentEditPoint(m_lastTimelineMs, 1), false); });
    // With an overlay clip selected, its keys (as K / Shift+K).
    const auto toKey = [this](int direction) {
        const TrackClip *o = selectedOverlayClip();
        navigateTo(m_timeline->adjacentKey(m_lastTimelineMs, direction, o ? o->id : 0), false);
    };
    addKey(play, tr("Previous / next keyframe: the selected overlay's, else the reframe ones"),
           {QKeySequence(Qt::SHIFT | Qt::Key_Up)}, [toKey] { toKey(-1); });
    addKey(play, QString(), {QKeySequence(Qt::SHIFT | Qt::Key_Down)}, [toKey] { toKey(1); });
    addKey(play, tr("Start / end of the timeline"), {QKeySequence(Qt::Key_Home)},
           [this] { navigateTo(0, false); });
    addKey(play, QString(), {QKeySequence(Qt::Key_End)},
           [this] { navigateTo(m_timeline->totalDurationMs(), false); });

    const QString edit = tr("Editing");
    addKey(edit, tr("Set the trim in / out point at the playhead"),
           {QKeySequence(Qt::Key_BracketLeft)}, [this] { trimAtPlayhead(true); });
    addKey(edit, QString(), {QKeySequence(Qt::Key_BracketRight)},
           [this] { trimAtPlayhead(false); });
    addKey(edit, tr("Restore: give the selected audio / overlay clip, else the video clip, its "
                    "full length"),
           {QKeySequence(Qt::Key_R)}, [this] { clearTrim(); });
    addKey(edit, tr("Split the selected audio / overlay clip, else the video clip, at the playhead"),
           {QKeySequence(Qt::Key_S)}, [this] { splitAtPlayhead(); });
    addKey(edit, tr("Delete the selected clip, else the reframe keyframe at the playhead"),
           {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)},
           [this] { deleteAtPlayhead(); });
    addKey(edit, tr("Clear the selection (or cancel drawing a Track subject / Blur a plate box)"),
           {QKeySequence(Qt::Key_Escape)}, [this] {
               if (m_trackSelecting) {
                   setTrackSelecting(false);
                   return;
               }
               if (m_plateSelecting) {
                   setPlateSelecting(false);
                   return;
               }
               selectTrackClip(-1);
               selectVideoClip(0);
           });

    const QString reframe = tr("Reframe");
    addKey(reframe, tr("Arm / disarm reframe authoring (drags and zooms set keyframes)"),
           {QKeySequence(Qt::Key_A)}, [this] { m_transport->toggleReframeArmed(); });
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    addKey(reframe, tr("Track subject: draw a box around it (again to cancel)"),
           {QKeySequence(Qt::Key_T)}, [this] { setTrackSelecting(!m_trackSelecting); });
#endif
    // With an overlay clip selected, K / Shift+K act on its keys instead (the
    // Overlay panel's Set key / Delete).
    addKey(reframe,
           tr("Set a keyframe at the playhead: the selected overlay's look, else the "
              "current view"),
           {QKeySequence(Qt::Key_K)}, [this] {
               if (selectedOverlayClip())
                   setOverlayKeyAtPlayhead();
               else if (m_clipId)
                   m_timeline->upsertClipReframeKey(m_clipId, m_lastPositionMs,
                                                    {m_viewport->Yaw(), m_viewport->Pitch(),
                                                     m_viewport->Roll(), m_viewport->FovY()});
           });
    addKey(reframe,
           tr("Delete the keyframe at the playhead: the selected overlay's, else the "
              "reframe one"),
           {QKeySequence(Qt::SHIFT | Qt::Key_K)}, [this] {
               if (selectedOverlayClip())
                   deleteOverlayKeyAtPlayhead();
               else if (m_clipId)
                   m_timeline->removeClipReframeKeyNear(m_clipId, m_lastPositionMs);
           });
    addKey(reframe,
           tr("Delete all keyframes: the selected overlay's, else the video clip's reframe ones"),
           {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K)}, [this] {
               if (selectedOverlayClip())
                   clearOverlayKeys();
               else if (m_clipId)
                   m_timeline->clearClipReframe(m_clipId);
           });

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    addKey(tr("Privacy"), tr("Blur a plate: draw a box around it (again to cancel or stop)"),
           {QKeySequence(Qt::Key_B)}, [this] { togglePlateBlur(); });
#endif

    // Timeline zoom (also Ctrl+wheel over the timeline).
    const QString view = tr("Timeline view");
    addKey(view, tr("Zoom in"),
           {QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)},
           [this] { m_timelineWidget->zoomIn(); });
    addKey(view, tr("Zoom out"), {QKeySequence(Qt::CTRL | Qt::Key_Minus)},
           [this] { m_timelineWidget->zoomOut(); });
    addKey(view, tr("Zoom to fit"), {QKeySequence(Qt::CTRL | Qt::Key_0)},
           [this] { m_timelineWidget->zoomToFit(); });

    // The sidebar's rail, top to bottom; like clicking its icon.
    const QString panels = tr("Settings panels");
    constexpr Qt::Key kDigits[] = {Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5,
                                   Qt::Key_6, Qt::Key_7, Qt::Key_8, Qt::Key_9};
    for (int i = 0; i < 9; ++i)
        addKey(panels,
               i == 0 ? tr("Show / hide a settings panel, top to bottom in the rail") : QString(),
               {QKeySequence(Qt::ALT | kDigits[i])}, [this, i] { m_sidebar->togglePanel(i); },
               i == 0 ? QKeySequence(Qt::ALT | Qt::Key_1).toString(QKeySequence::NativeText) +
                            QStringLiteral(" … ") +
                            QKeySequence(Qt::ALT | Qt::Key_9).toString(QKeySequence::NativeText)
                      : QString());

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *keysAction =
        helpMenu->addAction(tr("&Keyboard shortcuts"), this, &MainWindow::showShortcutHelp);
    keysAction->setShortcut(QKeySequence(Qt::Key_F1));
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About orbis-reframe"), this,
                        [this] { showAboutDialog(this, m_viewport); });
}

void MainWindow::showShortcutHelp()
{
    const auto keysText = [](const QList<QKeySequence> &keys) {
        QStringList parts;
        for (const QKeySequence &k : keys)
            parts << k.toString(QKeySequence::NativeText).toHtmlEscaped();
        return parts.join(QStringLiteral(" / "));
    };
    // Sections of (keys, what) rows, laid out in two columns below.
    struct Section {
        QString title;
        QList<std::pair<QString, QString>> rows;
    };
    QList<Section> sections;
    const auto row = [&sections](const QString &group, const QString &keys, const QString &what) {
        if (sections.isEmpty() || sections.back().title != group)
            sections.append({group, {}});
        sections.back().rows.append({keys, what});
    };

    // Shortcuts made by addKey(); an entry with no text pairs with the one
    // before it ("Previous / next frame": Left / Right).
    for (int i = 0; i < m_keyHelp.size(); ++i) {
        const KeyHelp &k = m_keyHelp[i];
        if (k.what.isEmpty())
            continue;
        QList<QKeySequence> keys = k.keys;
        for (int j = i + 1; j < m_keyHelp.size() && m_keyHelp[j].what.isEmpty(); ++j)
            keys += m_keyHelp[j].keys;
        row(k.group, k.keysLabel.isEmpty() ? keysText(keys) : k.keysLabel.toHtmlEscaped(), k.what);
    }
    // Menu actions, from the menus themselves.
    for (QAction *menuAction : menuBar()->actions()) {
        QMenu *menu = menuAction->menu();
        if (!menu)
            continue;
        const QString title =
            tr("%1 menu").arg(QString(menuAction->text()).remove(QLatin1Char('&')));
        for (QAction *a : menu->actions())
            if (!a->shortcut().isEmpty())
                row(title, keysText(a->shortcuts()),
                    QString(a->text()).remove(QLatin1Char('&')).remove(QStringLiteral("…")));
    }

    // Two columns, split between sections near half the rows.
    int total = 0;
    for (const Section &s : sections)
        total += s.rows.size() + 1;
    QString columns[2];
    int done = 0;
    for (const Section &s : sections) {
        QString &html = columns[done + (s.rows.size() + 1) / 2 > total / 2 ? 1 : 0];
        html += QStringLiteral("<tr><td colspan='2' style='padding-top:10px'><b>%1</b></td></tr>")
                    .arg(s.title.toHtmlEscaped());
        for (const auto &[keys, what] : s.rows)
            html += QStringLiteral("<tr><td style='padding-right:18px'><tt>%1</tt></td>"
                                   "<td>%2</td></tr>")
                        .arg(keys, what.toHtmlEscaped());
        done += s.rows.size() + 1;
    }
    const QString html =
        QStringLiteral("<table><tr><td valign='top' style='padding-right:36px'>"
                       "<table cellspacing='0' cellpadding='3'>%1</table></td>"
                       "<td valign='top'><table cellspacing='0' cellpadding='3'>%2</table>"
                       "</td></tr></table>")
            .arg(columns[0], columns[1]);

    // Scrolls when the screen is too small for it.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Keyboard shortcuts"));
    auto *label = new QLabel(html);
    label->setTextFormat(Qt::RichText);
    label->setContentsMargins(12, 0, 12, 12);
    auto *scroll = new QScrollArea(&dlg);
    scroll->setWidget(label);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(scroll, 1);
    layout->addWidget(buttons);
    const QSize want =
        label->sizeHint() + QSize(scroll->verticalScrollBar()->sizeHint().width(), 0);
    const QRect avail = screen()->availableGeometry();
    scroll->setMinimumSize(std::min(want.width(), avail.width() * 9 / 10),
                           std::min(want.height(), avail.height() * 8 / 10));
    dlg.exec();
}

void MainWindow::navigateTo(qint64 timelineMs, bool pause)
{
    if (m_timeline->isEmpty())
        return;
    timelineMs = std::clamp<qint64>(timelineMs, 0, m_timeline->totalDurationMs());
    if (pause && m_playing)
        QMetaObject::invokeMethod(m_controller, "pause", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, timelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->seek(timelineMs);
#endif
    m_lastTimelineMs = timelineMs;
    m_timelineWidget->setPlayhead(timelineMs);
    bindActiveClipAt(timelineMs);
}

void MainWindow::splitAtPlayhead()
{
    if (const TrackClip *a = m_selectedTrackClipId > 0
                                 ? m_timeline->trackClipById(m_selectedTrackClipId)
                                 : nullptr) {
        if (m_lastTimelineMs <= a->startMs || m_lastTimelineMs >= a->endMs()) {
            m_transport->showNotice(tr("Move the playhead into the selected clip to split it"),
                                    false);
            return;
        }
        if (const int right = m_timeline->splitTrackClip(a->id, m_lastTimelineMs))
            selectTrackClip(right);
        else
            m_transport->showNotice(tr("Too close to the clip's edge to split"), false);
        return;
    }
    if (!m_clipId)
        return;
    if (editClipId() != m_clipId) {
        m_transport->showNotice(tr("Move the playhead into the selected clip to split it"), false);
        return;
    }
    if (!m_timeline->splitClip(m_clipId, m_lastPositionMs))
        m_transport->showNotice(tr("Too close to the clip's edge to split"), false);
}

void MainWindow::deleteAtPlayhead()
{
    if (m_selectedTrackClipId > 0) {
        m_timeline->removeTrackClip(m_selectedTrackClipId);
        return;
    }
    if (m_selectedClipId > 0) {
        m_timeline->removeClip(m_selectedClipId);
        return;
    }
    if (m_clipId)
        m_timeline->removeClipReframeKeyNear(m_clipId, m_lastPositionMs);
}

void MainWindow::clearTrim()
{
    if (const TrackClip *a = m_selectedTrackClipId > 0
                                 ? m_timeline->trackClipById(m_selectedTrackClipId)
                                 : nullptr) {
        const int id = a->id;
        const qint64 full = a->sourceDurationMs;
        m_timeline->setTrackClipTrimIn(id, 0); // the start moves back with it
        m_timeline->setTrackClipTrimOut(id, full);
        return;
    }
    if (const int id = editClipId())
        m_timeline->clearClipTrim(id);
}

void MainWindow::detachOtherMic(int clipId)
{
    const int idx = m_timeline->indexOfId(clipId);
    if (idx < 0)
        return;
    const ClipItem &c = m_timeline->clipAt(idx);
    const int streams = audioStreamCount(c.sourcePath);
    if (streams < 2)
        return;
    // The clip plays its picked stream; the "other mic" is the first one it
    // doesn't.
    const int other = c.audioTrack == 0 ? 1 : 0;

    TrackClip a;
    a.sourcePath = c.sourcePath;
    a.sourceDurationMs = c.sourceDurationMs;
    a.stream = other;
    a.startMs = m_timeline->clipToTimeline(idx, 0);
    a.trimInMs = c.trimInMs;
    a.trimOutMs = c.trimOutMs;
    a.speed = c.speed; // stays in step with the picture, pitch kept
    a.linkedClipId = c.id; // and follows the clip's edits
    const qint64 endMs = a.startMs + c.timelineDurationMs();

    // Already on a track (over this footage): select it rather than add a
    // second copy that would play the stream twice.
    for (int i = 0; i < m_timeline->trackCount(); ++i)
        for (const TrackClip &x : m_timeline->trackAt(i).clips)
            if (x.stream == other && x.sourcePath == c.sourcePath && x.trimInMs < c.trimOutMs &&
                c.trimInMs < x.trimOutMs) {
                selectTrackClip(x.id);
                return;
            }

    // Onto a track that already holds detached camera audio, if one has room;
    // else a new "Mics" track -- keeping mics apart from music.
    auto holdsMics = [](const Track &t) {
        return std::any_of(t.clips.begin(), t.clips.end(),
                           [](const TrackClip &x) { return x.stream >= 0; });
    };
    int trackId = freeTrackFor(a.startMs, endMs, false, holdsMics);
    if (!trackId)
        trackId = m_timeline->addTrack(Track::Kind::Audio, tr("Mics"));
    selectTrackClip(m_timeline->insertTrackClip(trackId, a));
}

void MainWindow::putOverlaySoundOnTrack(int clipId)
{
    int trackId = 0;
    const TrackClip *o = m_timeline->trackClipById(clipId, &trackId);
    if (!o || m_timeline->trackById(trackId)->kind != Track::Kind::Video ||
        audioStreamCount(o->sourcePath) < 1)
        return;
    TrackClip a;
    a.sourcePath = o->sourcePath;
    a.sourceDurationMs = o->sourceDurationMs;
    a.startMs = o->startMs;
    a.trimInMs = o->trimInMs;
    a.trimOutMs = o->trimOutMs;
    a.speed = o->speed;
    a.linkedClipId = o->id; // and follows the overlay's edits
    const qint64 endMs = o->endMs();

    // Its sound is already on a track: select that rather than play it twice.
    for (int i = m_timeline->videoTrackCount(); i < m_timeline->trackCount(); ++i)
        for (const TrackClip &x : m_timeline->trackAt(i).clips)
            if (x.linkedClipId == o->id ||
                (x.sourcePath == o->sourcePath && x.trimInMs < o->trimOutMs &&
                 o->trimInMs < x.trimOutMs)) {
                selectTrackClip(x.id);
                return;
            }

    int audioTrack = freeTrackFor(a.startMs, endMs, false, [](const Track &) { return true; });
    if (!audioTrack)
        audioTrack = m_timeline->addTrack(Track::Kind::Audio);
    selectTrackClip(m_timeline->insertTrackClip(audioTrack, a));
}

void MainWindow::syncMediaInfoPanel(const ClipItem &c)
{
    auto it = m_mediaInfo.constFind(c.sourcePath);
    if (it == m_mediaInfo.constEnd()) {
        insv::MediaInfo info;
        std::string err;
        if (!insv::ProbeMediaInfo(c.sourcePath.toStdString(), info, err)) {
            m_mediaInfoPanel->setError(c.sourcePath, QString::fromStdString(err));
            return;
        }
        it = m_mediaInfo.insert(c.sourcePath, info);
    }
    m_mediaInfoPanel->setInfo(*it);
}

void MainWindow::pushTimelineToController()
{
    pushBlur(); // the clips' trims / sources may have changed
    QList<PlaybackController::Segment> segments;
    segments.reserve(m_timeline->count());
    qint64 total = 0;
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        segments.push_back(
            {c.sourcePath, c.trimInMs, c.trimOutMs, c.audioGainDb,
             c.audioMuted || m_timeline->mainAudioMuted(), c.audioTrack, c.speed, c.noise});
        total += c.timelineDurationMs();
    }
    m_totalTimelineMs = total;
    // Queued lambda (not a slot) so QList<Segment> needs no metatype.
    QMetaObject::invokeMethod(m_controller,
                              [ctrl = m_controller, segments] { ctrl->setTimeline(segments); });
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->setTimeline(segments); // GUI-thread resident -- no invokeMethod needed
#endif
}

void MainWindow::refreshTimelineWidget()
{
    QVector<TimelineWidget::Clip> clips;
    clips.reserve(m_timeline->count());
    bool activeHasKeys = false;
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        TimelineWidget::Clip w;
        w.id = c.id;
        w.label = QFileInfo(c.sourcePath).fileName();
        w.trimInMs = c.trimInMs;
        w.trimOutMs = c.trimOutMs;
        w.sourceDurationMs = c.sourceDurationMs;
        w.missing = c.mediaMissing;
        w.speed = c.speed;
        w.hasOtherAudioStream = audioStreamCount(c.sourcePath) > 1;
        if (!c.mediaMissing)
            w.peaks = waveformFor(c.sourcePath, c.audioTrack);
        w.gain = c.audioMuted || m_timeline->mainAudioMuted()
                     ? 0.0
                     : std::pow(10.0, c.audioGainDb / 20.0);
        for (qint64 t : c.reframe.keyTimes())
            w.keySourceMs.push_back(t);
        clips.push_back(std::move(w));
        if (c.id == m_clipId && !c.reframe.isEmpty())
            activeHasKeys = true;
    }
    m_timelineWidget->setClips(clips);
    m_timelineWidget->setMainMuted(m_timeline->mainAudioMuted());
    m_timelineWidget->setActiveClip(editClipId());
    refreshTrackRows();
    m_transport->setHasReframeKeys(activeHasKeys);
    m_reframePanel->setClipUnkeyed(m_timeline->clipById(m_clipId) && !activeHasKeys);
    if (!m_exporting)
        m_exportAction->setEnabled(m_timeline->count() > 0 && m_fps > 0.0);
    updateScreenshotEnabled();
}

void MainWindow::syncTimeline()
{
    pushTimelineToController();
    refreshTimelineWidget();
}

void MainWindow::remapPlayheadAfterEdit()
{
    // A trim / insert / remove / reorder changes the timeline<->source mapping.
    // Rebuild the controller's segments, then re-seek so the frame the user was
    // looking at (this clip's source time m_lastPositionMs) stays under the
    // playhead -- clamped into the clip's new [in, out] if the edit cut past it.
    if (m_rebindingModel || !m_clipId) {
        syncTimeline();
        return;
    }
    const int idx = m_timeline->indexOfId(m_clipId);
    if (idx < 0) { // the clip under the playhead is gone -- let the clamp settle it
        syncTimeline();
        return;
    }
    const ClipItem &c = m_timeline->clipAt(idx);
    const qint64 srcClamped = std::clamp(m_lastPositionMs, c.trimInMs, c.trimOutMs);
    const qint64 newTimelineMs = m_timeline->clipToTimeline(idx, c.localAtSourceMs(srcClamped));

    syncTimeline();

    m_lastPositionMs = srcClamped;
    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, newTimelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->seek(newTimelineMs);
#endif
}

void MainWindow::undo()
{
    if (!m_project->canUndo())
        return;
    m_rebindingModel = true; // suppress the storm of remapPlayheadAfterEdit calls this triggers
    m_project->undo();
    m_rebindingModel = false;
    afterUndoRedo();
}

void MainWindow::redo()
{
    if (!m_project->canRedo())
        return;
    m_rebindingModel = true;
    m_project->redo();
    m_rebindingModel = false;
    afterUndoRedo();
}

void MainWindow::afterUndoRedo()
{
    // undo()/redo() replace the whole Timeline (Project::applyDocument), so
    // every ClipItem has a fresh id -- rebind by the timeline position the
    // playhead was already at rather than trying to find the "same" clip id.
    syncTimeline();
    bindActiveClipAt(m_lastTimelineMs);
    m_transport->setPosition(m_lastTimelineMs);
    m_timelineWidget->setPlayhead(m_lastTimelineMs);
    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, m_lastTimelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->seek(m_lastTimelineMs);
#endif
}

void MainWindow::addClipToTimeline(int index)
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Add clip to timeline"),
        m_clipPath.isEmpty() ? QDir::homePath() : QFileInfo(m_clipPath).absolutePath(),
        tr("Insta360 video (*.insv);;All files (*)"));
    if (path.isEmpty())
        return;

    stitch::ClipProbe probe;
    std::string probeErr;
    if (!stitch::ProbeClip(path.toStdString(), probe, probeErr)) {
        QMessageBox::warning(this, tr("Could not add clip"),
                             probeErr.empty() ? tr("Unrecognised file: %1").arg(path)
                                              : QString::fromStdString(probeErr));
        return;
    }
    const bool wasEmpty = m_timeline->count() == 0;
    m_timeline->insertClip(index < 0 ? m_timeline->count() : index, path, probe.duration_ms,
                           probe.fps, probe.width, probe.height);
    if (wasEmpty) {
        m_clipPath = path;
        m_fps = probe.fps;
    }
}

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
    if (!stitch::ProbeClip(clipPath.toStdString(), probe, probeErr)) {
        QMessageBox::warning(this, tr("Could not open clip"),
                             probeErr.empty() ? tr("Unrecognised file: %1").arg(clipPath)
                                              : QString::fromStdString(probeErr));
        return;
    }
    m_clipPath = clipPath;
    m_fps = probe.fps;
    m_rebindingModel = true;
    m_clipId = m_project->resetWithClip(clipPath, probe.duration_ms, probe.fps, probe.width,
                                        probe.height);
    m_rebindingModel = false;
    if (const ClipItem *c = m_timeline->clipById(m_clipId)) {
        syncAudioPanel(*c);
        syncMediaInfoPanel(*c);
        applyColorGrade(c->color);
        m_colourPanel->setState(c->color);
        syncPrivacyPanel();
    }
    syncTimeline(); // opens paused on the first frame
}

void MainWindow::openProject(const QString &projectPath)
{
    QString error;
    m_rebindingModel = true;
    const bool loaded = m_project->load(projectPath, &error);
    m_rebindingModel = false;
    if (!loaded) {
        QMessageBox::warning(this, tr("Could not open project"), error);
        return;
    }
    if (m_timeline->count() == 0) {
        m_clipId = 0;
        m_clipPath.clear();
        m_fps = 0.0;
        m_audioPanel->setNoClip();
        m_colourPanel->setNoClip();
        m_privacyPanel->setNoClip();
        m_mediaInfoPanel->setNoClip();
        syncTimeline(); // empty -> controller goes idle, widget clears
        return;
    }

    const ClipItem &first = m_timeline->clipAt(0);
    m_clipId = first.id;
    m_clipPath = first.sourcePath;
    m_fps = first.fps;
    syncAudioPanel(first);
    syncMediaInfoPanel(first);
    applyColorGrade(first.color);
    m_colourPanel->setState(first.color);
    syncPrivacyPanel();

    if (first.mediaMissing) {
        // Keep the loaded model on screen but don't try to play a missing file.
        QMessageBox::warning(this, tr("Missing media"),
                             tr("A clip's source file was not found:\n%1").arg(first.sourcePath));
        syncTimeline();
        return;
    }
    syncTimeline(); // opens paused on the first frame
}

void MainWindow::saveProject()
{
    if (m_project->filePath().isEmpty()) {
        saveProjectAs();
        return;
    }
    QString error;
    if (!m_project->save(m_project->filePath(), &error))
        QMessageBox::warning(this, tr("Could not save project"), error);
}

void MainWindow::saveProjectAs()
{
    const QString suggested =
        m_project->filePath().isEmpty()
            ? QDir::homePath() + QStringLiteral("/untitled.%1").arg(Project::kFileExtension)
            : m_project->filePath();
    QString path = askSaveFileName(
        tr("Save project as"), suggested,
        tr("orbis-reframe project (*.%1)").arg(Project::kFileExtension));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".%1").arg(Project::kFileExtension);
    QString error;
    if (!m_project->save(path, &error))
        QMessageBox::warning(this, tr("Could not save project"), error);
}

bool MainWindow::confirmDiscardChanges()
{
    if (!m_project->modified())
        return true;
    // Qt's Save / Discard labels have no mnemonic; with one, S / D (and
    // Alt+S / Alt+D) press them. Enter saves, Esc cancels.
    QMessageBox box(QMessageBox::Question, tr("Unsaved changes"),
                    tr("The project has unsaved changes. Save them?"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.button(QMessageBox::Save)->setText(tr("&Save"));
    box.button(QMessageBox::Discard)->setText(tr("&Discard"));
    box.setDefaultButton(QMessageBox::Save);
    box.setEscapeButton(QMessageBox::Cancel);
    const auto choice = box.exec();
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save) {
        saveProject();
        return !m_project->modified(); // a cancelled Save-As leaves it modified
    }
    return true; // Discard
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
    QString name = m_project && !m_project->filePath().isEmpty()
                       ? QFileInfo(m_project->filePath()).fileName()
                       : tr("untitled");
    const QString star =
        (m_project && m_project->modified()) ? QStringLiteral(" *") : QString();
    setWindowTitle(QStringLiteral("orbis-reframe — %1%2").arg(name, star));
}

void MainWindow::exportFlatVideo()
{
    if (m_exporting)
        return;
    const int nclips = m_timeline->count();
    if (nclips == 0)
        return;
    // Checked first: without NVENC the export would only fail once FFmpeg
    // opens the encoder, with its own error text.
    if (!syscheck::nvencAvailable()) {
        QMessageBox::warning(this, tr("Export flat video"), syscheck::nvencMissingText());
        return;
    }

    // One output frame rate for the whole timeline: the project's (Project
    // Settings), or clip 0's when it's "match first clip". Each clip is
    // conformed to it -- frames dropped or repeated by source time, at the
    // clip's speed (ReframeExportController).
    const double fps = m_project->output().effectiveFps(m_timeline->clipAt(0).fps);

    // A clip with the privacy blur on is only exported once its whole
    // trimmed range has been scanned for plates.
    for (int i = 0; i < nclips; ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        if (const QString why = privacyBlocker(c, c.trimInMs, c.trimOutMs); !why.isEmpty()) {
            QMessageBox::warning(this, tr("Export flat video"), why);
            return;
        }
    }
    {
        QStringList stale;
        for (int i = 0; i < nclips; ++i) {
            const ClipItem &c = m_timeline->clipAt(i);
            if (const QString w = privacyStaleness(c, c.trimInMs, c.trimOutMs, true); !w.isEmpty())
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
                                   QDir::homePath() + QStringLiteral("/timeline.mp4"),
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
    req.bitrateKbps = opts.bitrateKbps();
    req.lensGuard = m_project->previewStitch().lensGuard;
    req.depthSeam = m_project->previewStitch().depthSeam;

    // Per clip: the reframe track and the built stabilizer; the export samples
    // both per decoded frame at its own source time.
    for (int i = 0; i < nclips; ++i)
        req.clips.push_back(exportJobFor(m_timeline->clipAt(i)));
    req.mix = mixClips(); // the extra audio tracks
    req.insets = insetJobs(); // the overlay tracks

    // The export opens its own NativeStitchSource (on the shared decode
    // device) -- pause playback so the two stitch pipelines don't contend
    // for the GPU.
    QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);

    auto *thread = new QThread(this);
    auto *worker = new ReframeExportController;
    worker->moveToThread(thread);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);

    auto *dlg = new QProgressDialog(tr("Preparing…"), tr("Cancel"), 0, 100, this);
    dlg->setWindowTitle(tr("Exporting flat video"));
    dlg->setWindowModality(Qt::ApplicationModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setValue(0);

    m_exporting = true;
    m_exportAction->setEnabled(false);
    updateScreenshotEnabled();

    // Cancel: flip the worker's atomic flag (thread-safe -- a lone atomic
    // store, so calling it straight from the GUI thread is fine), and hold the
    // dialog up with a "Cancelling…" note until the worker stops and finished()
    // closes it. The worker deletes the partial output file itself. Context is
    // `dlg` (GUI thread), NOT `worker` -- the worker thread is blocked in run().
    connect(dlg, &QProgressDialog::canceled, dlg, [worker, dlg] {
        worker->cancel();
        dlg->setLabelText(tr("Cancelling…"));
        dlg->show();
    });

    connect(worker, &ReframeExportController::progress, dlg, [dlg](int pct, const QString &label) {
        if (dlg->wasCanceled())
            return;
        if (!label.isEmpty())
            dlg->setLabelText(label);
        dlg->setValue(pct);
    });
    connect(worker, &ReframeExportController::finished, this,
            [this, dlg, thread](bool ok, const QString &error, const QString &outPath) {
                dlg->close();
                dlg->deleteLater();
                thread->quit();
                thread->wait();
                thread->deleteLater();
                m_exporting = false;
                m_exportAction->setEnabled(m_timeline->count() > 0 && m_fps > 0.0);
                updateScreenshotEnabled();
                if (ok)
                    QMessageBox::information(this, tr("Export complete"),
                                            tr("Wrote %1").arg(outPath));
                else if (!error.isEmpty())
                    QMessageBox::warning(this, tr("Export failed"), error);
                // else: cancelled by the user -- the dialog just closes, and
                // ReframeExportController has already deleted the partial file.
            });

    thread->start();
    QMetaObject::invokeMethod(worker, [worker, req] { worker->run(req); });
}

ReframeExportController::ClipJob MainWindow::exportJobFor(const ClipItem &c) const
{
    // Stabilization (Lean follow / Horizon Lock / Custom) is a per-frame
    // sphere-frame matrix next to the reframe pose, applied after it like the
    // viewport's pc.stab -- the native stitch itself is never stabilized.
    const bool nativeStab = m_stabConfig.mode != insv::StabMode::Off;
    ReframeExportController::ClipJob job;
    job.clipPath = c.sourcePath;
    job.trimInMs = c.trimInMs;
    job.trimOutMs = c.trimOutMs;
    job.speed = c.speed;
    const std::array<float, 4> grade = c.color.packed();
    std::copy(grade.begin(), grade.end(), job.colorGrade);
    job.audioTrack = c.audioTrack;
    job.audioGainDb = c.audioGainDb;
    job.audioMuted = c.audioMuted || m_timeline->mainAudioMuted();
    job.noise = c.noise;
    job.blur = blurFor(c);
    seamAdjustFor(c.sourcePath, job.seamAdjustDeg);
    if (nativeStab) {
        auto tele = insv::ReadInsvTelemetry(c.sourcePath.toStdString());
        if (tele && tele->imu.size() >= 2) {
            auto stab = std::make_shared<insv::GyroStabilizer>();
            const double syncMs = std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs;
            stab->build(*tele, c.fps, m_stabConfig, syncMs / 1000.0);
            if (stab->ready())
                job.stab = stab;
        }
    }
    // No keyframes -> a static forward view.
    if (!c.reframe.isEmpty())
        job.poseAt = [track = c.reframe](qint64 sourceMs, float pose[4]) {
            const ReframePose p = track.sample(sourceMs);
            pose[0] = p.yaw;
            pose[1] = p.pitch;
            pose[2] = p.roll;
            pose[3] = p.fovY;
        };
    return job;
}

namespace {

// The clip's removed blur tracks as they fall in `scan` (null when none).
std::shared_ptr<const privacy::RemovedSpans> resolveRemovals(const ClipItem &c,
                                                              const privacy::PlateScan &scan)
{
    if (c.blurRemovals.empty())
        return {};
    auto r = std::make_shared<privacy::RemovedSpans>(scan.resolveRemovals(c.blurRemovals));
    return r->empty() ? nullptr : r;
}

} // namespace

stitch::BlurQuadsFn MainWindow::blurFor(const ClipItem &c) const
{
    // The scan's plates (with "Blur number plates" on), and the ones blurred
    // by hand either way.
    PlateScanCache::Scan scan = c.blurPlates ? m_plateScans.get(c.sourcePath) : nullptr;
    if (!scan && c.blurAdditions.empty())
        return {};
    std::shared_ptr<const privacy::RemovedSpans> removed;
    if (scan)
        removed = resolveRemovals(c, *scan);
    const double fps = c.fps;
    return [scan, removed, added = c.blurAdditions, fps](int64_t ptsMs, std::vector<insv::BlurQuad> &out) {
        if (scan)
            scan->quadsAt(ptsMs, out, removed.get());
        privacy::addedQuadsAt(added, ptsMs, fps, out);
    };
}

QString MainWindow::privacyBlocker(const ClipItem &c, qint64 fromMs, qint64 toMs) const
{
    if (!c.blurPlates)
        return {};
    const PlateScanCache::Scan scan = m_plateScans.get(c.sourcePath);
    if (scan && scan->covers(fromMs, toMs))
        return {};
    const QString name = QFileInfo(c.sourcePath).fileName();
    return scan ? tr("“%1” has the privacy blur on, but only part of it has been scanned for "
                     "plates. Scan the clip in the Privacy panel first.")
                      .arg(name)
                : tr("“%1” has the privacy blur on, but it hasn't been scanned for plates yet. "
                     "Scan the clip in the Privacy panel first.")
                      .arg(name);
}

bool MainWindow::captureTimingOn() const
{
    // As ensureStabilizerFor pushes it to playback, and exportJobFor to the
    // export.
    return m_stabConfig.mode != insv::StabMode::Off;
}

QString MainWindow::privacyStaleness(const ClipItem &c, qint64 fromMs, qint64 toMs, bool name) const
{
    const bool on = captureTimingOn();
    bool scanStale = false;
    if (c.blurPlates)
        if (const PlateScanCache::Scan scan = m_plateScans.get(c.sourcePath))
            scanStale = scan->staleOver(fromMs, toMs, on);
    int handStale = 0;
    for (const privacy::BlurAddition &a : c.blurAdditions)
        if (!a.frames.empty() && a.frames.back().tMs >= fromMs && a.frames.front().tMs <= toMs &&
            privacy::timingStale(a.timing, on))
            ++handStale;
    if (!scanStale && !handStale)
        return {};
    // Found with the correction = with the stabilization on.
    const QString then = on ? tr("off") : tr("on");
    QString what;
    if (scanStale && handStale)
        what = tr("The plate scan and %n plate(s) blurred by hand were found with the stabilization "
                  "%1",
                  nullptr, handStale)
                   .arg(then);
    else if (scanStale)
        what = tr("The plate scan was made with the stabilization %1").arg(then);
    else
        what = tr("%n plate(s) blurred by hand were followed with the stabilization %1", nullptr, handStale)
                   .arg(then);
    QString text = tr("%1, which stitches the picture slightly differently: the blurs may sit a "
                      "little off the plates in fast swings. Check them with \"Show regions\", "
                      "or redo them (scan again, blur the plate again) -- or set the "
                      "stabilization back %2.")
                       .arg(what, then);
    if (name)
        text = QStringLiteral("“%1”: ").arg(QFileInfo(c.sourcePath).fileName()) + text;
    return text;
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
    // Per source: the trim ranges of its clips with the blur on -- playback
    // works per source, and two clips of one file may differ.
    QHash<QString, QList<BlurRange>> ranges;
    QHash<QString, QList<BlurRange>> allRanges; // the overlay's: every clip
    QSet<QString> sources;
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        sources.insert(c.sourcePath);
        BlurRange r;
        r.fromMs = c.trimInMs;
        r.toMs = c.trimOutMs;
        r.scan = c.blurPlates;
        r.fps = c.fps;
        if (const PlateScanCache::Scan scan = m_plateScans.get(c.sourcePath))
            r.removed = resolveRemovals(c, *scan);
        if (!c.blurAdditions.empty())
            r.added = std::make_shared<const std::vector<privacy::BlurAddition>>(c.blurAdditions);
        allRanges[c.sourcePath].append(r);
        if (c.blurPlates || r.added)
            ranges[c.sourcePath].append(r);
    }
    pushRegionOverlay(allRanges);
    for (const QString &path : sources) {
        if (!ranges.contains(path)) {
            m_controller->setBlurQuads(path, {});
            continue;
        }
        m_controller->setBlurQuads(
            path, [scan = m_plateScans.get(path), r = ranges.value(path)](int64_t ptsMs,
                                                                          std::vector<insv::BlurQuad> &out) {
                for (const BlurRange &x : r)
                    if (ptsMs >= x.fromMs - 50 && ptsMs <= x.toMs + 50) {
                        if (scan && x.scan)
                            scan->quadsAt(ptsMs, out, x.removed.get());
                        if (x.added)
                            privacy::addedQuadsAt(*x.added, ptsMs, x.fps, out);
                        return;
                    }
            });
    }
}

void MainWindow::pushRegionOverlay(const QHash<QString, QList<BlurRange>> &ranges)
{
    // "Show regions": the scan's regions of every clip with a scan, blur on or
    // not, in the privacy spike's debug colours (by RegionKind; the Privacy
    // panel's legend matches); the ones removed by hand in grey.
    struct Source {
        PlateScanCache::Scan scan;
        QList<BlurRange> ranges;
    };
    QHash<QString, Source> scanned;
    if (m_showPlateRegions)
        for (auto it = ranges.begin(); it != ranges.end(); ++it) {
            PlateScanCache::Scan scan = m_plateScans.get(it.key());
            const bool added = std::any_of(it.value().begin(), it.value().end(),
                                           [](const BlurRange &r) { return r.added != nullptr; });
            if (scan || added)
                scanned.insert(it.key(), {std::move(scan), it.value()});
        }
    if (scanned.isEmpty()) {
        m_controller->setOutlineProvider({});
        return;
    }
    m_controller->setOutlineProvider(
        [scanned](const QString &path, qint64 sourceMs,
                  std::vector<EquirectViewportWindow::RegionOutline> &out) {
            const auto it = scanned.find(path);
            if (it == scanned.end())
                return;
            const auto r = std::find_if(it->ranges.begin(), it->ranges.end(), [sourceMs](const BlurRange &x) {
                return sourceMs >= x.fromMs - 50 && sourceMs <= x.toMs + 50;
            });
            if (r == it->ranges.end())
                return;
            // Detected, Filled, Tracked, Unverified; removed; added by hand.
            static constexpr float kColour[4][3] = {
                {0.0f, 1.0f, 0.0f}, {1.0f, 0.86f, 0.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 1.0f}};
            static constexpr float kRemoved[3] = {0.55f, 0.55f, 0.55f};
            static constexpr float kAdded[3] = {0.35f, 0.6f, 1.0f};
            if (r->added)
                for (const privacy::BlurAddition &a : *r->added)
                    if (const privacy::BlurAddition::Frame *af = privacy::addedFrameAt(a, sourceMs, r->fps)) {
                        EquirectViewportWindow::RegionOutline o;
                        std::memcpy(o.corner, af->corner, sizeof(o.corner));
                        std::copy_n(kAdded, 3, o.rgb);
                        out.push_back(o);
                    }
            const privacy::ScanFrame *f = it->scan ? it->scan->frameAt(sourceMs) : nullptr;
            if (!f)
                return;
            for (const privacy::BlurRegion &reg : f->regions) {
                EquirectViewportWindow::RegionOutline o;
                std::memcpy(o.corner, reg.quad.corner, sizeof(o.corner));
                const bool removed = privacy::PlateScan::isRemoved(reg, f->tMs, r->removed.get());
                std::copy_n(removed ? kRemoved : kColour[std::min<int>(static_cast<int>(reg.kind), 3)], 3,
                            o.rgb);
                out.push_back(o);
            }
        });
}

void MainWindow::showPictureMenu(const QPoint &globalPos, const std::array<float, 3> &dir)
{
    // The clip on screen and the frame shown (paused: the playhead's).
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c)
        return;
    const qint64 t = m_lastPositionMs;
    const int clipId = c->id;
    // A plate blurred by hand (always shown; on top of the scan's).
    if (const int ai = privacy::addedAt(c->blurAdditions, t, c->fps, dir.data());
        ai >= 0) {
        QMenu menu;
        auto edit = [this, clipId, ai](qint64 fromMs) {
            const ClipItem *c = m_timeline->clipById(clipId);
            if (!c || ai >= int(c->blurAdditions.size()))
                return;
            std::vector<privacy::BlurAddition> all = c->blurAdditions;
            auto &frames = all[ai].frames;
            frames.erase(std::lower_bound(frames.begin(), frames.end(), fromMs,
                                          [](const privacy::BlurAddition::Frame &f, qint64 t) {
                                              return f.tMs < t;
                                          }),
                         frames.end());
            if (frames.empty())
                all.erase(all.begin() + ai);
            m_timeline->setClipBlurAdditions(clipId, all);
        };
        menu.addAction(tr("Remove this blur"), this, [edit] { edit(INT64_MIN); })
            ->setToolTip(tr("A plate blurred by hand: drop it in every frame."));
        menu.addAction(tr("Remove this blur from here on"), this, [edit, t] { edit(t - 1); });
        menu.exec(globalPos);
        return;
    }
    if (!c->blurPlates && !m_showPlateRegions)
        return; // no blur and no outlines: nothing to point at
    const PlateScanCache::Scan scan = m_plateScans.get(c->sourcePath);
    if (!scan)
        return;
    const privacy::ScanFrame *f = scan->frameAt(t);
    if (!f)
        return;
    const auto removed = resolveRemovals(*c, *scan);
    // The topmost region under the pointer; a removed one only while the
    // outlines show it.
    const privacy::BlurRegion *hit = nullptr;
    for (auto it = f->regions.rbegin(); it != f->regions.rend() && !hit; ++it)
        if (privacy::quadContains(it->quad, dir.data()) &&
            (m_showPlateRegions || !privacy::PlateScan::isRemoved(*it, f->tMs, removed.get())))
            hit = &*it;
    if (!hit)
        return;
    const int32_t track = hit->track;
    QMenu menu;
    if (privacy::PlateScan::isRemoved(*hit, f->tMs, removed.get())) {
        menu.addAction(tr("Restore this blur"), this, [this, clipId, track, t] {
            const ClipItem *c = m_timeline->clipById(clipId);
            const PlateScanCache::Scan scan = c ? m_plateScans.get(c->sourcePath) : nullptr;
            if (!scan)
                return;
            // Drop every removal that takes this track away here.
            std::vector<privacy::BlurRemoval> keep;
            for (const privacy::BlurRemoval &rm : c->blurRemovals) {
                const privacy::RemovedSpans spans = scan->resolveRemovals({rm});
                if (std::none_of(spans.begin(), spans.end(), [track, t](const privacy::RemovedSpan &s) {
                        return s.track == track && t >= s.fromMs && t <= s.toMs;
                    }))
                    keep.push_back(rm);
            }
            m_timeline->setClipBlurRemovals(clipId, keep);
        });
    } else {
        auto remove = [this, clipId, track](qint64 fromMs) {
            const ClipItem *c = m_timeline->clipById(clipId);
            const PlateScanCache::Scan scan = c ? m_plateScans.get(c->sourcePath) : nullptr;
            if (!scan)
                return;
            privacy::BlurRemoval rm = scan->describeRemoval(track, fromMs);
            if (rm.path.empty())
                return;
            std::vector<privacy::BlurRemoval> all = c->blurRemovals;
            all.push_back(std::move(rm));
            m_timeline->setClipBlurRemovals(clipId, all);
            m_transport->showNotice(tr("Blur removed. Right-click it with Show regions on to "
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
    if (!c) {
        m_privacyPanel->setNoClip();
        return;
    }
    const QString cantScan = syscheck::plateScanUnavailableReason();
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const bool canScan = cantScan.isEmpty() && !c->mediaMissing && !plateJobRunning();
#else
    const bool canScan = false;
#endif
    const PlateScanCache::Scan scan = m_plateScans.get(c->sourcePath);
    const bool covered = scan && scan->covers(c->trimInMs, c->trimOutMs);
    QString status;
    if (covered) {
        int frames = 0, withPlates = 0;
        for (const privacy::ScanFrame &f : scan->frames)
            if (f.tMs >= c->trimInMs && f.tMs < c->trimOutMs) {
                ++frames;
                withPlates += f.regions.empty() ? 0 : 1;
            }
        status = withPlates ? tr("Scanned: plates on %1 of %2 frames.").arg(withPlates).arg(frames)
                            : tr("Scanned: no plates found.");
    } else if (scan && std::any_of(scan->spans.begin(), scan->spans.end(), [c](const auto &s) {
                   return s.toMs >= c->trimInMs && s.fromMs <= c->trimOutMs;
               })) {
        status = tr("Partly scanned. Scan the clip to cover all of it.");
    } else {
        status = tr("Not scanned yet.");
    }
    if (c->blurPlates && !covered)
        status += QLatin1Char(' ') + tr("Exports with the blur on are refused until it is.");
    if (!c->blurAdditions.empty())
        status += QLatin1Char(' ') + tr("%n plate(s) blurred by hand.", nullptr, int(c->blurAdditions.size()));
    if (!cantScan.isEmpty())
        status += QLatin1Char(' ') + cantScan;
    m_privacyPanel->setState(c->blurPlates, status, canScan, static_cast<int>(c->blurRemovals.size()));
    m_privacyPanel->setWarning(privacyStaleness(*c, c->trimInMs, c->trimOutMs, false));
}

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
platescan::Request MainWindow::plateRequestFor(const ClipItem &c)
{
    // The stitch as playback / export set it up for this clip.
    platescan::Request req;
    req.clipPath = c.sourcePath.toStdString();
    req.fromMs = c.trimInMs;
    req.toMs = c.trimOutMs;
    req.fps = c.fps;
    req.syncAdjustMs = std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs;
    seamAdjustFor(c.sourcePath, req.seamAdjustDeg);
    req.lensGuard = m_project->previewStitch().lensGuard;
    req.depthSeam = m_project->previewStitch().depthSeam;
    req.captureTiming = captureTimingOn();
    return req;
}
#endif

void MainWindow::setPlateSelecting(bool on)
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (on && (!c || c->mediaMissing || plateJobRunning() || trackingRunning() || m_exporting))
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
        m_transport->showNotice(tr("Drag a box around the plate to blur (Esc cancels). It is "
                                   "followed from this frame on, then back from it."),
                                true);
    } else {
        m_viewport->ClearMarker();
        m_transport->showNotice(QString());
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
    if (!c || c->mediaMissing || plateJobRunning() || trackingRunning() || m_exporting) {
        m_viewport->ClearMarker();
        return;
    }
    const qint64 from = m_lastPositionMs, to = c->trimOutMs, back = c->trimInMs;
    if (to - from < 100 && from - back < 100) {
        m_viewport->ClearMarker();
        m_transport->showNotice(tr("The clip is too short."));
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
    m_transport->showNotice(tr("Following the plate. Stop following in the Privacy panel ends it."), true);
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
        m_transport->showNotice(QString());
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
        m_transport->showNotice(
            tr("Blurred the plate for %1 s (%2 s of it before the box), up to where it was lost "
               "-- the playhead is there. Blur a plate again to pick it up.")
                .arg(secs, before),
            true);
    else
        m_transport->showNotice(tr("Blurred the plate for %1 s (%2 s of it before the box).")
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
    m_plateJobFromMs = c->trimInMs;
    m_plateJobToMs = c->trimOutMs;
    m_plateJobs->startScan(plateRequestFor(*c));
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
    if (!r.scan.empty()) {
        QString err;
        if (!m_plateScans.store(source, r.scan, &err))
            QMessageBox::warning(this, tr("Scan for plates"),
                                 tr("The scan couldn't be saved (%1); it holds until the "
                                    "app closes.")
                                     .arg(err));
    }
    pushBlur();
    syncPrivacyPanel();
    if (!m_playing)
        QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                  Q_ARG(qint64, m_lastTimelineMs));
    m_transport->showNotice(r.stopped ? tr("Plate scan stopped; the part scanned is kept.")
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

void MainWindow::updateScreenshotEnabled()
{
    const bool on = !m_screenshotThread && !m_exporting && m_timeline->count() > 0 && m_fps > 0.0;
    m_screenshotAction->setEnabled(on);
    m_transport->setScreenshotEnabled(on);
    // While tracking, the button is the Stop control.
    m_reframePanel->setTrackEnabled(trackingRunning() ||
                                    (!m_exporting && m_timeline->count() > 0 && m_fps > 0.0));
}

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
    if (on && (!m_clipId || trackingRunning() || m_exporting))
        on = false;
    m_trackSelecting = on;
    m_reframePanel->setTrackSelecting(on);
    m_viewport->SetBoxSelectMode(on);
    if (on) {
        // A still frame to draw on.
        QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
        m_viewport->ClearMarker();
        m_viewport->requestActivate(); // so Esc reaches it
        m_transport->showNotice(tr("Drag a box around the subject to track (Esc cancels)."), true);
    } else {
        if (!trackingRunning())
            m_viewport->ClearMarker();
        m_transport->showNotice(QString());
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
    if (!c || trackingRunning() || m_exporting) {
        m_viewport->ClearMarker();
        return;
    }
    const qint64 from = m_lastPositionMs, to = c->trimOutMs;
    if (to - from < 500) {
        m_viewport->ClearMarker();
        m_transport->showNotice(tr("Too close to the end of the clip to track."));
        return;
    }

    // The box -> the subject's direction and angular size, in the stabilized
    // frame the reframe keys live in (the viewport's pose is applied before
    // the stab matrix, so the pose alone maps the box).
    const float aspect = m_viewport->OutputAspect() > 0.0f
                             ? m_viewport->OutputAspect()
                             : float(m_viewport->width()) / std::max(1, m_viewport->height());
    const ReframePose look{m_viewport->Yaw(), m_viewport->Pitch(), m_viewport->Roll(),
                           m_viewport->FovY()};
    SubjectTrackController::Run run;
    subjecttrack::Request &req = run.req;
    req.seed = track::seedFromViewBox(box.x(), box.y(), box.width(), box.height(), look.yaw,
                                      look.pitch, look.roll, look.fovY, aspect);
    const ReframeExportController::ClipJob job = exportJobFor(*c); // the stab, as exported
    req.clipPath = c->sourcePath.toStdString();
    req.fromMs = from;
    req.toMs = to;
    req.fps = c->fps;
    req.stab = job.stab;
    req.seamAdjustDeg = job.seamAdjustDeg;
    req.blur = job.blur;
    req.lensGuard = m_project->previewStitch().lensGuard;
    run.clipId = c->id;
    run.look = look;
    run.aspect = aspect;

    // It plays what it tracks in the viewport instead: pause playback, which
    // would push frames too (and its own NativeStitchSource would contend).
    QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
    m_tracker->start(run);
    m_reframePanel->setTrackRunning(true);
    m_reframePanel->setTrackStatus(tr("Starting…"));
    m_transport->showNotice(tr("Tracking. Space, Esc or Stop tracking ends it."), true);
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
        m_transport->showNotice(QString());
        settle(from);
        QMessageBox::warning(this, tr("Track subject"),
                             tr("Tracking failed: %1").arg(QString::fromStdString(r.error)));
        return;
    }
    if (r.samples.size() < 2) {
        settle(from);
        m_transport->showNotice(
            tr("Lost the subject straight away. Try again with a box that fits it closely."));
        return;
    }
    const std::vector<track::PathKey> keys = track::reduceToKeys(r.samples, run.req.fps);
    QList<ReframeKey> list;
    for (const track::PathKey &k : keys)
        list.append(ReframeKey{k.tMs, {float(k.yaw), float(k.pitch), look.roll, look.fovY}});
    const qint64 until = r.samples.back().tMs;
    m_timeline->replaceClipReframeSpan(clipId, from, until, list); // one undo step
    settle(until);
    const QString secs = QString::number((until - from) / 1000.0, 'f', 1);
    if (r.endedLost)
        m_transport->showNotice(
            tr("Tracked %1 s (%2 keyframes), up to where the subject was last seen -- "
               "the playhead is there to track on from.")
                .arg(secs)
                .arg(list.size()),
            true);
    else
        m_transport->showNotice(
            tr("Tracked %1 s (%2 keyframes).").arg(secs).arg(list.size()));
}
#endif

void MainWindow::takeScreenshot()
{
    if (m_screenshotThread || m_exporting)
        return;
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c)
        return;
    if (const QString why = privacyBlocker(*c, m_lastPositionMs, m_lastPositionMs); !why.isEmpty()) {
        QMessageBox::warning(this, tr("Screenshot"), why);
        return;
    }
    // A frame either side: the playhead's time is not the shown frame's.
    const qint64 frameMs = std::llround(1000.0 / c->fps);
    if (const QString w = privacyStaleness(*c, m_lastPositionMs - frameMs, m_lastPositionMs + frameMs, true);
        !confirmStalePrivacy(tr("Screenshot"), w.isEmpty() ? QStringList() : QStringList{w}))
        return;

    // Captured before the save dialog, so a playing timeline doesn't move the
    // frame on: the export's view of the clip, looked at through whatever the
    // viewport shows now -- the sampled reframe path, or a free look / drag.
    const ReframeExportController::ClipJob job = exportJobFor(*c);
    const std::array<float, 4> pose{m_viewport->Yaw(), m_viewport->Pitch(), m_viewport->Roll(),
                                    m_viewport->FovY()};
    const qint64 sourceMs = m_lastPositionMs;
    const int w = m_project->output().width;
    const int h = m_project->output().height;
    const auto guard = m_project->previewStitch().lensGuard;
    const bool depthSeam = m_project->previewStitch().depthSeam;

    // Suggested in the project's folder, or Pictures while it's unsaved; named
    // after the project (or the clip) and the playhead's timeline time, with a
    // -2, -3, ... suffix if that name is taken.
    QString dir;
    QString base;
    if (!m_project->filePath().isEmpty()) {
        const QFileInfo fi(m_project->filePath());
        dir = fi.absolutePath();
        base = fi.completeBaseName();
    } else {
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (dir.isEmpty() || !QDir(dir).exists())
            dir = QDir::homePath();
        base = QFileInfo(c->sourcePath).completeBaseName();
    }
    const qint64 t = std::max<qint64>(0, m_lastTimelineMs);
    const QString stamp = QStringLiteral("%1m%2s%3")
                              .arg(t / 60000, 2, 10, QLatin1Char('0'))
                              .arg(t / 1000 % 60, 2, 10, QLatin1Char('0'))
                              .arg(t % 1000, 3, 10, QLatin1Char('0'));
    QString suggested = QDir(dir).filePath(QStringLiteral("%1_%2.png").arg(base, stamp));
    for (int n = 2; QFileInfo::exists(suggested); ++n)
        suggested = QDir(dir).filePath(QStringLiteral("%1_%2-%3.png").arg(base, stamp).arg(n));

    QString path = askSaveFileName(tr("Save screenshot"), suggested, tr("PNG image (*.png)"));
    if (path.isEmpty() || m_screenshotThread || m_exporting)
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".png");

    m_transport->showNotice(tr("Saving screenshot…"), true);
    // The overlays showing at the playhead, composited like the export does.
    std::vector<exportpipe::InsetClipJob> insets = insetJobs();
    m_screenshotThread = QThread::create([this, job, pose, sourceMs, w, h, guard, depthSeam, path, t,
                                          insets = std::move(insets)] {
        QString err;
        QImage img =
            ReframeExportController::renderStill(job, sourceMs, pose.data(), w, h, guard,
                                                 depthSeam, err);
        if (!img.isNull() && !insets.empty()) {
            exportpipe::InsetCompositor comp(insets, w, h);
            std::string e;
            if (!comp.compositeRgb(t, img.bits(), int(img.bytesPerLine()), 3, e))
                err = QString::fromStdString(e);
        }
        if (img.isNull() && err.isEmpty())
            err = tr("The frame couldn't be rendered.");
        else if (!img.isNull() && err.isEmpty() && !img.save(path, "PNG"))
            err = tr("Couldn't write %1").arg(path);
        QMetaObject::invokeMethod(this, [this, err, path] {
            m_screenshotThread->wait(); // its function has just returned
            m_screenshotThread->deleteLater();
            m_screenshotThread = nullptr;
            updateScreenshotEnabled();
            if (err.isEmpty()) {
                m_transport->showNotice(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
            } else {
                m_transport->showNotice(QString());
                QMessageBox::warning(this, tr("Screenshot failed"), err);
            }
        }, Qt::QueuedConnection);
    });
    m_screenshotThread->start();
    updateScreenshotEnabled();
}
