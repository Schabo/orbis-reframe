#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QString>

#include <atomic>
#include <functional>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <GyroStabilizer.h> // insv::GyroStabilizer / StabConfig
#include <AudioMixer.h>      // insv::MixClip
#include <InsvMediaInfo.h>   // insv::MediaInfo

#include "PlateScanCache.h"           // PlateScanCache (privacy blur)
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
#include "PlateScanJob.h"             // platescan::Request, Result, FollowResult
#endif
#ifdef ORBIS_REFRAME_HAVE_TRACKING
#include "SubjectTrackJob.h"          // subjecttrack::Result
#endif
#include "ReframeExportController.h" // ReframeExportController::ClipJob
#include "../core/ClipItem.h"         // ColorGrade (the copied grade)
#include "../core/InsetGeometry.h"    // inset::Look (overlays)

class QAction;
class QCloseEvent;
class QShortcut;
class QStackedWidget;
class QThread;
class QVulkanInstance;
class Project;
class ProjectLibrary;
class ProjectLibraryPage;
class Timeline;
struct ClipItem;
class EquirectViewportWindow;
class PlaybackController;
class PlateJobController;
class SubjectTrackController;
class AudioPlayer;
class AudioPanel;
class ColourPanel;
class PrivacyPanel;
class MediaInfoPanel;
class OverlayPanel;
class ProjectPanel;
class ReframePanel;
class MediaPoolPanel;
class SettingsSidebar;
class TabSidebar;
class StitchingPanel;
class StabilizationPanel;
class TimelineWidget;
class WaveformCache;
class TransportBar;
struct Track;
struct TrackClip;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QVulkanInstance *vulkanInstance, QWidget *parent = nullptr);
    ~MainWindow() override;

    EquirectViewportWindow *viewport() const { return m_viewport; }

    // Open a path given on the command line: a *.orp project file loads the
    // project, anything else is treated as a loose source clip.
    void openPath(const QString &path);
    // Probe a bare source clip, make it a fresh one-clip project, and play it.
    void openClip(const QString &clipPath);
    // Load a project file and start playing its timeline.
    void openProject(const QString &projectPath);
    // The start screen (no project open): the project library.
    void showLibrary();

protected:
    void closeEvent(QCloseEvent *event) override;
    // Installed on the application: a text/number field (the panels' spin
    // boxes) claims Space, [ ], K, Delete... while it has focus, so the
    // editor's shortcuts go dead. A click anywhere outside the field --
    // viewport, timeline, empty panel space -- or Enter / Escape in it hands
    // focus back (the field commits its value on focus-out).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // File -> "Export flat video…": bake every timeline clip's reframe path,
    // then a ReframeExportController on a scratch QThread (options dialog ->
    // save-path dialog -> modal QProgressDialog) renders the whole timeline to
    // one flat .mp4.
    void exportFlatVideo();
    // One timeline clip as the export renders it: trim, speed, grade, audio,
    // seam fine-tune, a stabilizer built from its IMU track (when
    // stabilization is on), and its reframe path.
    ReframeExportController::ClipJob exportJobFor(const ClipItem &c) const;
    // Transport bar camera button / Ctrl+Shift+P: render the frame at the
    // playhead at the project output size, through the export path (same
    // stitch, stabilization, grade; the viewport's current view), on a scratch
    // thread, and save it as a PNG where a save dialog says (suggested in the
    // project's folder, or Pictures when the project is unsaved).
    void takeScreenshot();
    void updateScreenshotEnabled();
    // Track subject (Reframe panel; ORBIS_REFRAME_HAVE_TRACKING): arm / disarm
    // drawing a box in the viewport; a drawn box starts tracking from the
    // playhead, and the path lands in the clip's ReframeTrack as keys.
    void setTrackSelecting(bool on);
    void startSubjectTracking(const QRectF &box);
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    // The tracked path -> the clip's reframe keys; the playhead to where it
    // ended.
    void finishSubjectTracking(const subjecttrack::Result &r);
#endif
    // A Track subject run is going (always false without tracking).
    bool trackingRunning() const;
    // Point the viewport along the authored reframe path. `sourceMs` is in the
    // current clip's (m_clipId) source time. No-op when the track is empty or a
    // viewport drag is in progress.
    void applySampledPose(qint64 sourceMs);
    // Build a stabilizer for clip `c` (its source's IMU track under its own
    // stabilization setting and the source's sync fine-tune) into
    // m_stabCache, unless one is there already or the clip's stabilization is
    // Off. Reads the clip's trailer (fast: it seeks to the trailer, not the
    // whole file). Returns true if it built one; the caller then publishes.
    bool ensureStabilizerFor(const ClipItem &c);
    // The clip under the playhead and the one after it: build their
    // stabilizers (so a cut finds the next one ready), publish, and refresh
    // the paused picture if the current one was new.
    void ensureStabilizers();
    // The measured sync fine-tune for a source (ClipItem::syncAdjustMs),
    // NaN if not measured. maybeMeasureSync() starts a background measurement
    // (stitch::MeasureClipSync) when the source has none; the result lands on
    // the Timeline, which rebuilds the stabilizer.
    double syncAdjustFor(const QString &clipPath) const;
    void maybeMeasureSync(const QString &clipPath);
    void updateSyncStatus();
    // The seam alignment fine-tune for a source
    // (ClipItem::seamAdjustDeg), if it was measured under the current
    // lens-guard setting. pushSeamAdjust() hands every timeline source's value
    // (or zero) to the playback controller; maybeMeasureSeam() starts a
    // background measurement (stitch::MeasureClipSeam) when the source has none.
    bool seamAdjustFor(const QString &clipPath, std::array<double, 3> &deg) const;
    void pushSeamAdjust();
    void maybeMeasureSeam(const QString &clipPath);
    void updateSeamStatus();
    // Start whichever of those two measurements is due next, for the
    // timeline's sources in order (one of each kind at a time). Runs after the
    // current event (a load or add finishes first) and never while playing:
    // a measurement competes with playback for the GPU and shows as a hitch,
    // so they run when a clip is added or a project opened, and a pause picks
    // the queue back up. One running when play starts is cancelled (it would
    // stall playback its whole run) and starts over on the pause; a result
    // that still lands while playing is applied at the next pause (the
    // re-stitch is a seek).
    void scheduleClipScans();
    // Push the current clip's (m_clipId) correction for `sourceMs` (its
    // source time) into the viewport. Identity when its stabilization is Off
    // or its stabilizer isn't built.
    void applyStabilization(qint64 sourceMs);
    // Copy the viewport's current pose into the Reframe panel's numeric fields
    // (whatever's on screen -- sampled path, free-look, or a drag result).
    void syncReframePanel();
    // Push a clip's colour grade into the viewport. Small helper since
    // every call site has a ColorGrade struct but SetColorGrade takes a flat
    // float[4].
    void applyColorGrade(const ColorGrade &grade);
    // Bind authoring (m_clipId / m_lastPositionMs / stabilizer / viewport pose
    // / Audio+Colour panels) to whichever clip covers `timelineMs`. Shared by
    // the playback position tick and undo()/redo() (which replace the whole
    // Timeline, so m_clipId can't just be reused -- rebind by position).
    void bindActiveClipAt(qint64 timelineMs);
    // Undo/redo: Project::undo()/redo() rebuild the whole Timeline (new
    // clip ids throughout), so this resyncs everything downstream of the
    // model by timeline position rather than trying to preserve m_clipId.
    void undo();
    void redo();
    void afterUndoRedo();

    // Rebuild the controller's segment list from the timeline (also updates
    // m_totalTimelineMs). Queued to the worker thread.
    void pushTimelineToController();
    // Reflect clip `c` in the Audio panel, including how many audio streams
    // its source has (probed once per path, cached in m_audioTrackCounts).
    void syncAudioPanel(const ClipItem &c);
    // Audio streams in `path`, cached per source.
    int audioStreamCount(const QString &path);
    // A stream's waveform for the timeline (null until scanned -- the scan
    // starts here and refreshTimelineWidget runs when it lands).
    std::shared_ptr<const std::vector<float>> waveformFor(const QString &path, int stream);
    // Extra tracks: push them to the timeline widget; select an audio clip
    // (-1 = none); react to any change to them.
    void refreshTrackRows();
    void selectTrackClip(int clipId);
    // The video clip selected by a click on the timeline (0 = none: the
    // panels then follow the playhead). The panels (Audio, Colour, Media
    // info) and the trim buttons edit editClipId(): the selected clip, else
    // the one under the playhead. Reframe keys and the viewport stay with the
    // clip under the playhead (m_clipId).
    void selectVideoClip(int clipId);
    const ClipItem *selectedVideoClip() const;
    int editClipId() const;
    void refreshClipPanels();
    // Reflect the panel's clip's (editClipId()) stabilization in the
    // Stabilization panel; greyed out without one.
    void syncStabilizationPanel();
    // Privacy blur (Privacy panel): the panel for the clip being edited; the
    // blur regions handed to playback, per source, for its clips with the
    // blur on; a clip's for export (empty when off / not scanned); why a clip
    // can't be exported blurred over [fromMs, toMs] (empty = it can); the
    // plate scan of the clip being edited.
    void syncPrivacyPanel();
    // A clip's trim range on its source, with the blur tracks removed from it
    // resolved against the source's scan.
    struct BlurRange {
        qint64 fromMs = 0, toMs = 0;
        bool scan = false; // the clip's "Blur number plates"
        std::shared_ptr<const privacy::RemovedSpans> removed;
        std::shared_ptr<const std::vector<privacy::BlurAddition>> added; // null = none
        double fps = 30.0;
    };
    void pushBlur();
    void pushRegionOverlay(const QHash<QString, QList<BlurRange>> &ranges);
    stitch::BlurQuadsFn blurFor(const ClipItem &c) const;
    // Right click on the picture: remove / restore the blur track under it
    // (manual correction).
    void showPictureMenu(const QPoint &globalPos, const std::array<float, 3> &dir);
    QString privacyBlocker(const ClipItem &c, qint64 fromMs, qint64 toMs) const;
    // Whether the stitch has the capture-time correction for `c` (its
    // stabilization on) -- the one a new scan / followed plate is found in.
    static bool captureTimingOn(const ClipItem &c);
    // A warning when `c`'s blurs over [fromMs, toMs] were found in a stitch
    // other than today's, so they may sit off the plates; empty when they
    // match. `name` = start with the clip's file name.
    QString privacyStaleness(const ClipItem &c, qint64 fromMs, qint64 toMs, bool name) const;
    // Before an export / screenshot: false when a clip's blur is stale and
    // the user cancels.
    bool confirmStalePrivacy(const QString &title, const QStringList &warnings);
    void startPlateScan();
    // "Blur a plate…": arm / disarm the box select for it; follow the plate
    // in a drawn box (output-rect fractions) from the playhead on.
    void setPlateSelecting(bool on);
    // The "Blur a plate…" button / B: arm it, cancel the box, or stop a
    // running follow.
    void togglePlateBlur();
    void startPlateFollow(const QRectF &box);
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    platescan::Request plateRequestFor(const ClipItem &c);
    // A finished scan -> the scan cache; a finished follow -> the clip's
    // blur additions.
    void finishPlateScan(const platescan::Result &r);
    void finishPlateFollow(const platescan::FollowResult &r);
#endif
    // A plate scan or follow is running / is a follow; stop it (all no-ops
    // without the plate scan).
    bool plateJobRunning() const;
    bool plateFollowing() const;
    void stopPlateJob();

    void onTracksChanged();
    // The extra audio tracks as the mixer wants them (muted tracks / clips
    // and missing files left out). Playback and export both use this.
    std::vector<insv::MixClip> mixClips() const;
    // The Audio panel: the selected audio clip if there is one, else the
    // clip under the playhead.
    void refreshAudioPanel();
    // Pick an audio file and put it at `atMs` on track `trackId` (0 = the
    // first track with room there, or a new one).
    void addAudioFile(int trackId, qint64 atMs);
    // Put `paths` at `atMs` on track `trackId` (0 = the first with room for
    // all of them, or a new one), one after another. Files without audio are
    // reported and skipped. Used by the file picker and by drops.
    void addAudioFiles(const QStringList &paths, int trackId, qint64 atMs);
    // The first audio track (`video`: overlay track) that has nothing in
    // [startMs, endMs) and that `accept` takes; 0 if none.
    int freeTrackFor(qint64 startMs, qint64 endMs, bool video,
                     const std::function<bool(const Track &)> &accept) const;
    // Overlays: pick video files / put `paths` at `atMs` on overlay track
    // `trackId` (0 = the first with room, or a new one), one after another.
    // Files without a video stream are reported and skipped.
    void addVideoFile(int trackId, qint64 atMs);
    void addVideoFiles(const QStringList &paths, int trackId, qint64 atMs);
    // The overlay clips as InsetFeed / the export want them (hidden tracks
    // and missing files left out), bottom-most first.
    void pushInsetsToController();
    std::vector<exportpipe::InsetClipJob> insetJobs() const;
    // The selected track clip if it is on an overlay track (else null).
    const TrackClip *selectedOverlayClip(int *trackId = nullptr) const;
    // The Overlay panel: the selected overlay clip, at the playhead.
    void refreshOverlayPanel();
    // The selected overlay clip's keys at the playhead (the Overlay panel's
    // Set key / Delete, and K / Shift+K with an overlay selected): set one
    // with its look there (the playhead must be over it), or delete the one
    // within Timeline::kInsetKeyDeleteSnapMs. Clear (Ctrl+Shift+K): delete
    // them all, leaving the look at the playhead.
    void setOverlayKeyAtPlayhead();
    void deleteOverlayKeyAtPlayhead();
    void clearOverlayKeys();
    // Set an overlay clip's look (position / size / opacity): a key at the
    // playhead if it has keys (the playhead must be over it), else its one
    // look.
    void applyInsetLook(int clipId, const inset::Look &look);
    // Put the clip's other audio stream (camera mic vs. Mic Air) on an audio
    // track as its own clip -- same span, speed and trim, own level / mute /
    // position for a sync nudge.
    void detachOtherMic(int clipId);
    // Put an overlay clip's sound on an audio track, linked to it so it
    // follows its moves, trims, splits and removal.
    void putOverlaySoundOnTrack(int clipId);
    // The video clip an audio clip would link to: a main-track or
    // overlay clip of the same footage, overlapping source range, nearest on
    // the timeline. 0 if none.
    int linkTargetFor(const TrackClip &a) const;
    // The trim buttons / [ ] keys: the selected audio clip if there is one
    // (at the playhead's point in it), else the video clip under the playhead.
    void trimAtPlayhead(bool in);
    void clearTrim();

    // Keyboard shortcuts. Every single-key / navigation shortcut goes
    // through addKey(), which also lists it in Help -> Keyboard shortcuts
    // (menu actions are listed from the menus themselves).
    void setupKeyboardShortcuts();
    // `keysLabel` replaces the keys' own names in the list ("Alt+1 … Alt+9").
    void addKey(const QString &group, const QString &what, const QList<QKeySequence> &keys,
                const std::function<void()> &fn, const QString &keysLabel = QString());
    void showShortcutHelp();
    // Seek the playhead from the keyboard. Updates the cached position at
    // once, so a held arrow key steps from where the last press went rather
    // than from a position the worker hasn't reported yet.
    void navigateTo(qint64 timelineMs, bool pause);
    // S: split the selected audio / overlay clip at the playhead if there is
    // one, else the video clip under the playhead.
    void splitAtPlayhead();
    // Delete: the selected audio / overlay clip, else the selected video
    // clip, else the reframe keyframe at the playhead.
    void deleteAtPlayhead();
    // Reflect clip `c`'s source file in the Media information panel (probed
    // once per path, cached in m_mediaInfo; a failed probe isn't cached).
    void syncMediaInfoPanel(const ClipItem &c);
    // Repaint the timeline widget from the model + refresh the transport
    // bar's total / reframe-button state + update the export action.
    void refreshTimelineWidget();
    // pushTimelineToController + refreshTimelineWidget.
    void syncTimeline();
    // syncTimeline + re-seek so the playhead stays on the same source frame
    // after a trim / structural edit. Connected to every structural / trim
    // Timeline signal.
    void remapPlayheadAfterEdit();
    // File dialog -> probe -> Timeline::insertClip at `index` (-1 = append).
    void addClipToTimeline(int index = -1);
    // Probe -> insert `paths` in order from `index` (-1 = append). Files that
    // aren't clips are reported and skipped. Used by the picker and by drops.
    void addClipFiles(const QStringList &paths, int index);

    // Media pool (the left sidebar's Media tab). Import files… / Import
    // folder…: pick, then importMedia(). importMedia probes `paths` (a folder:
    // the files in it) on worker threads and adds the ones it can read; the
    // ones it can't are reported when the last is done.
    void importMediaFiles();
    void importMediaFolder();
    void importMedia(const QStringList &paths);
    // Add pool files to the timeline by kind: an .insv at the end of the
    // video track, flat video onto an overlay track and audio onto an audio
    // track at the playhead (the first track with room, or a new one).
    void addMediaToTimeline(const QStringList &paths);
    // Mark the files the timeline uses in the Media tab.
    void updateMediaInUse();

    // File menu project actions.
    // New project…: name, location and output format, then a project folder
    // with the (empty) project saved in it.
    void newProject();
    // Open project…: any project file, also one outside the library.
    void openProjectFile();
    // Project library…: close the project (asking to save) and show the
    // start screen.
    void closeToLibrary();
    // The editor in the window instead of the library; the editor's actions
    // and shortcuts work only while it's shown.
    void showEditor();
    void setEditorActive(bool active);
    // No clip: the panels' no-clip state, the controller idle.
    void bindEmptyTimeline();
    // Plate scans and waveforms in the project folder's cache/ (the user
    // cache for an untitled or loose project) of `projectFile`; on every
    // file path change, and before a project loads.
    void updateCacheDirs(const QString &projectFile);
    // Saved or opened: to the top of the library.
    void noteProjectOpened();
    void saveProject();
    // Save into a new project folder (also how an untitled project and a
    // loose project file get one).
    void saveProjectAs();
    // Prompt to save if the project is modified. Returns false if the user
    // cancelled (the caller should abort whatever it was about to do).
    bool confirmDiscardChanges();
    void updateWindowTitle();
    // QFileDialog::getSaveFileName with a "&Save" button (Alt+S / S in the
    // file list): Qt's own label has no mnemonic. `path` may name the file.
    QString askSaveFileName(const QString &caption, const QString &path, const QString &filter);

    // The window's two pages: the library (start screen) and the editor.
    QStackedWidget *m_pages = nullptr;
    ProjectLibraryPage *m_libraryPage = nullptr;
    QWidget *m_editorPage = nullptr;
    std::unique_ptr<ProjectLibrary> m_library;
    // Off while the library is shown (they'd act on the hidden editor).
    QList<QAction *> m_editorActions;
    // The left sidebar (sources) and its Media tab.
    TabSidebar *m_sourcesSidebar = nullptr;
    MediaPoolPanel *m_mediaPanel = nullptr;
    int m_mediaImportsPending = 0; // files still being probed
    QStringList m_mediaImportFailures;
    // Bumped whenever another project (or the library) is shown: imports
    // still being probed for the one before are dropped.
    int m_projectSession = 0;
    QList<QShortcut *> m_editorShortcuts;

    EquirectViewportWindow *m_viewport = nullptr;
    TimelineWidget *m_timelineWidget = nullptr;
    TransportBar *m_transport = nullptr;
    SettingsSidebar *m_sidebar = nullptr;
    ProjectPanel *m_projectPanel = nullptr;
    StitchingPanel *m_stitchingPanel = nullptr;
    StabilizationPanel *m_stabilizationPanel = nullptr;
    ReframePanel *m_reframePanel = nullptr;
    AudioPanel *m_audioPanel = nullptr;
    QHash<QString, int> m_audioTrackCounts; // source path -> audio stream count
    ColourPanel *m_colourPanel = nullptr;
    OverlayPanel *m_overlayPanel = nullptr;
    MediaInfoPanel *m_mediaInfoPanel = nullptr;
    QHash<QString, insv::MediaInfo> m_mediaInfo; // source path -> probe
    // The edit model: a Project owning a Timeline. Authoring (trim via [ ],
    // reframe via K / viewport drag) targets m_clipId, the clip currently
    // under the playhead (0 == nothing open); the timeline widget's own
    // edge-drag / context menu act on whichever clip was grabbed. m_timeline
    // is a cached m_project->timeline().
    Project *m_project = nullptr;
    Timeline *m_timeline = nullptr;
    int m_clipId = 0;
    int m_selectedTrackClipId = -1; // an audio clip selected on the timeline
    int m_selectedClipId = 0;       // a video clip selected on the timeline
    QString m_lastAudioDir;         // where the last audio file was picked from
    QString m_lastVideoDir;         // ... and the last overlay video
    QString m_lastMediaDir;         // ... and the last media imported
    WaveformCache *m_waveforms = nullptr;

    // Native gyro stabilization, per clip (ClipItem::stabilization). Built
    // stabilizers are kept per source file + setting + sync fine-tune, so
    // clips cut from one file with the same setting share one; the clip under
    // the playhead and the next one are built ahead (ensureStabilizers()).
    struct BuiltStab {
        QString path;
        insv::StabConfig config;
        double syncMs = 0.0;
        std::shared_ptr<const insv::GyroStabilizer> stab; // null: no IMU track
    };
    static constexpr size_t kStabCacheSize = 4;
    std::vector<BuiltStab> m_stabCache; // least recently used first
    // The cache entry for clip `c`, or null (stabilization Off, or not built).
    const BuiltStab *builtStabFor(const ClipItem &c) const;
    // Presented frames get their matrix from m_stabShared (per frame, by the
    // frame's clip, at its own source time, on the playback thread), the
    // stitch its capture-time correction from setCaptureTimings; the cache
    // itself also serves the paused / settings-changed refresh.
    struct StabShared {
        std::mutex mx;
        // Per clip id: its stabilizer, or null for identity (Off, or no IMU
        // track). A clip not in it isn't built yet: the viewport keeps the
        // matrix it has.
        std::unordered_map<int, std::shared_ptr<const insv::GyroStabilizer>> clips;
    };
    std::shared_ptr<StabShared> m_stabShared = std::make_shared<StabShared>();
    // Hand every clip's built stabilizer (and its capture-time correction)
    // to playback. Call after the cache or the clips change.
    void publishStabilizer();
    // Per clip id: whether its capture-time correction was on (its
    // stabilization on) when last seen, to tell when a change toggles it.
    // Rebuilt with the controller's segments.
    QHash<int, bool> m_captureTimingOn;
    // Background sync measurement: one at a time.
    QThread *m_syncThread = nullptr;
    QString m_syncMeasuringPath;
    std::atomic<bool> m_syncCancel{false};
    QSet<QString> m_syncFailed; // sources that gave no result this session
    QString m_syncLastMessage;  // last measurement's summary, for the panel
    // Background seam measurement: one at a time.
    QThread *m_seamThread = nullptr;
    QString m_seamMeasuringPath;
    std::atomic<bool> m_seamCancel{false};
    QSet<QString> m_seamFailed; // sources that gave no result this session
    QString m_seamLastMessage;
    bool m_scanQueued = false; // scheduleClipScans() already pending
    bool m_playing = false;    // the controller's playingChanged
    // A sync / seam result arrived while playing; applied on the next pause.
    bool m_measuredWhilePlaying = false;
    // Total trimmed timeline length, cached from the last pushTimelineToController.
    qint64 m_totalTimelineMs = 0;

    PlaybackController *m_controller = nullptr;
    QThread *m_playbackThread = nullptr;
    // Audio playback, only when the app was built with Qt6::Multimedia (see
    // CMakeLists.txt) -- a slave of m_controller's clock, not a separate
    // playback path; see AudioPlayer's own comment.
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    AudioPlayer *m_audioPlayer = nullptr;
#endif
    QAction *m_exportAction = nullptr;
    // The clip under the playhead, remembered so the export can re-open it
    // independently of playback; empty until a clip opens. Plus its frame rate.
    QString m_clipPath;
    double m_fps = 0.0;
    bool m_exporting = false;
    QAction *m_screenshotAction = nullptr;
    QThread *m_screenshotThread = nullptr; // one render at a time
    SubjectTrackController *m_tracker = nullptr; // Track subject (null without tracking)
    PrivacyPanel *m_privacyPanel = nullptr;
    mutable PlateScanCache m_plateScans; // read lazily, also from const exportJobFor
    // Plate scan or "Blur a plate…" follow, one at a time (null without the
    // plate scan). The source / start of the one running, for its handlers.
    PlateJobController *m_plateJobs = nullptr;
    QString m_plateJobSource;
    int m_plateJobClipId = 0;
    qint64 m_plateJobFromMs = 0, m_plateJobToMs = 0;
    bool m_plateSelecting = false;       // "Blur a plate…" is waiting for its box
    bool m_showPlateRegions = false; // Privacy panel "Show regions" (a view setting)
    bool m_trackSelecting = false;
    // Last playhead position, in the current clip's source time, so the
    // [ / ] / K shortcuts can act "at the playhead" without querying the
    // worker thread.
    qint64 m_lastPositionMs = 0;
    // Last playhead position in *timeline* ms (unlike m_lastPositionMs, valid
    // independently of m_clipId) -- undo()/redo() rebuild every ClipItem with
    // a fresh id, so afterUndoRedo() rebinds from this instead.
    qint64 m_lastTimelineMs = 0;
    QAction *m_undoAction = nullptr;
    // The grade from Copy grade (Colour panel / a clip's right-click menu);
    // kept for the session, not in the project.
    std::optional<ColorGrade> m_copiedGrade;
    std::optional<insv::StabConfig> m_copiedStab; // Copy / Paste stabilization
    struct KeyHelp {
        QString group;
        QString what;
        QList<QKeySequence> keys;
        QString keysLabel;
    };
    QList<KeyHelp> m_keyHelp; // shortcuts made by addKey(), in order
    QAction *m_redoAction = nullptr;
    // Reframe authoring armed (drag/zoom writes a keyframe); viewport drag in
    // progress (suspend pushing the sampled pose).
    bool m_reframeArmed = false;
    bool m_viewportDragging = false;
    // True while openClip()/openProject() are rebuilding the model, so the
    // clipTrimChanged handler doesn't try to remap the playhead against a
    // half-built timeline / a stale m_clipId.
    bool m_rebindingModel = false;
};
