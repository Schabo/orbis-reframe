#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>

#include <functional>
#include <QString>
#include <QTimer>

#include <IStitchSource.h>  // stitch::PreviewStitchConfig (POD; no FFmpeg)

#include "OutputSettings.h"

class MediaPool;
struct MediaItem;
class QDir;
class Timeline;

// The whole editing session: one Timeline, where it is saved, and whether it
// has unsaved changes. Owns the Timeline. Serialises to / from a JSON project
// file (*.orp).
//
// No FFmpeg dependency: load() trusts the source metadata persisted in
// the file (duration / fps / resolution) rather than re-probing, and marks a
// clip whose file is gone with ClipItem::mediaMissing instead of failing.
// (A re-probe-on-load pass to catch a replaced source can be layered on
// later, caller-side.)
class Project : public QObject {
    Q_OBJECT

public:
    explicit Project(QObject *parent = nullptr);

    // The file format. kFormatVersion loads, and kOldestFormatVersion up to
    // it (an older file is converted on load, see applyDocument); others are
    // refused. A field may be added without a bump when absent => today's
    // behaviour; anything an older build would misread bumps the version.
    // Optional fields are written only when set.
    //   "format": kFormatTag, "version": kFormatVersion,
    //   "previewStitch": {quality: "template" | "optflow", width, height
    //     (0 = auto), lensGuard: "none" | "standard" | "premium", depthSeam},
    //   "output": {aspect: "16:9" | "9:16" | "1:1" | "4:3", width, height,
    //     fps (0 = match the first clip)},
    //   "timeline": {clips: [...], tracks: [...]},
//   "media": [{source, kind: "360" | "video" | "audio", durationMs, width,
//     height, fps}] -- the media pool (absent => only what the timeline uses).
    // A main-track clip: source, trimInMs, trimOutMs, sourceDurationMs, fps,
    //   sourceWidth, sourceHeight, reframe {keys: [{tMs, yaw, pitch, roll,
    //   fovY}]}, color {exposureEv, contrast, saturation, temperature},
    //   speed, stabilization {mode, windowYawSec, windowPitchSec,
    //   windowRollSec, horizonLockAmount, horizonGain, accelLowPassHz,
    //   pitchOffsetRad, rollOffsetRad, autoLevel (absent => true)};
    //   optional: syncAdjustMs and seamAdjust {deg: [yaw,
    //   pitch, roll], lensGuard} once measured, and privacy:
    //     "plates": true -- the blur is on;
    //     "removed" -- blur tracks removed by hand, [{fromMs, toMs,
    //       wholeTrack, path: [[tMs, x, y, z], ...]}] (privacy::BlurRemoval);
    //     "added" -- plates blurred by hand, [{frames: [[tMs, kind, 12 corner
    //       coordinates in millionths], ...], timing: "corrected" |
    //       "uncorrected" | "noGyro"}] (privacy::BlurAddition).
    // A track: {kind: "audio" | "video", name, gainDb, muted, clips}; a
    //   stream track also has role: "cameraMic" | "micAir" (Track::Role).
    //   An audio clip: source, stream, sourceDurationMs, startMs, trimInMs,
    //   trimOutMs, speed, gainDb, muted, fadeInMs, fadeOutMs; optional: noise
    //   {mode: "wind" (motorcycle) | "windGeneral" | "hiss" | "voice" |
    //   "voiceAmbience", strength}, and when linked "linkedClip" (main-track index) or "linkedOverlay"
    //   [track index, clip index], + syncOffsetMs.
    //   A video (overlay) clip: the audio clip's timing fields + sourceWidth,
    //   sourceHeight, inset {centerX, centerY, width, opacity, cornerRadius,
    //   borderWidth, borderColor "#rrggbb"}, insetKeys [{tMs, centerX,
    //   centerY, width, opacity}].
    // Version 8 (0.1.0) had one "stabilization" object at the top level for
    // the whole project instead of one per clip. Before version 10 a clip
    // played one of its audio streams itself (audioTrack, with audioGainDb,
    // audioMuted, noise, and "mainMuted" on the timeline); on load that
    // becomes linked clips on the stream tracks, the unpicked streams muted.
    static constexpr int kFormatVersion = 10;
    static constexpr int kOldestFormatVersion = 8;
    static constexpr const char *kFormatTag = "orbis-reframe-project";
    static constexpr const char *kFileExtension = "orp"; // no dot

    Timeline *timeline() const { return m_timeline; }
    // The project's media (the Media tab). Holds every source the timeline
    // uses as well: a clip added to the timeline is added here too.
    MediaPool *mediaPool() const { return m_mediaPool; }
    // Empty until the project is saved or loaded.
    QString filePath() const { return m_filePath; }
    bool modified() const { return m_modified; }

    // Project-wide preview-stitch settings (Stitching panel). The live
    // playback path is handed this via PlaybackController::setPreviewConfig.
    const stitch::PreviewStitchConfig &previewStitch() const { return m_previewStitch; }

    // Project-wide output format (Project Settings panel): the aspect the
    // viewport is pinned to, the default export size, and the export rate.
    const OutputSettings &output() const { return m_output; }

    // Undo/redo (whole-project snapshots -- see the .cpp's top-of-file
    // comment for why and how edits are coalesced into one step per
    // "settled" user action).
    bool canUndo() const { return m_pendingDirty || !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

    // How many audio streams a source file has (InsvAudioDemux::
    // countAudioStreams; the model has no FFmpeg). Used to convert a project
    // from before version 10, whose clips' sound has to be put on the stream
    // tracks. Unset, or for a missing file: as many as the clip's picked
    // stream needs.
    void setAudioStreamCounter(std::function<int(const QString &)> counter)
    {
        m_audioStreamCounter = std::move(counter);
    }

public slots:
    // Empty timeline, no file, not modified.
    void newProject();

    // Replace the timeline with a single loose clip -- opening a bare .insv,
    // not a project file. Clears the file path and the modified flag. Returns
    // the new clip's id.
    int resetWithClip(const QString &sourcePath, qint64 durationMs, double fps,
                      int width = 0, int height = 0, int audioStreams = 0);

    // Write the timeline to `path` as JSON. On success records `path` as the
    // current file and clears modified. Returns false with *error set on
    // failure.
    bool save(const QString &path, QString *error = nullptr);

    // Parse `path` and rebuild the timeline from it. The existing timeline is
    // left untouched if the file is malformed. On success records `path` and
    // clears modified. Returns false with *error set on failure.
    bool load(const QString &path, QString *error = nullptr);

    // Replace the preview-stitch settings. Marks the project modified and
    // emits previewStitchChanged when the value actually changes.
    void setPreviewStitch(const stitch::PreviewStitchConfig &config);

    // A moved or renamed media file found again: `from` becomes `to` in the
    // media pool and in every clip cut from it. One undo step, like any edit.
    // Returns how many timeline clips changed.
    int relinkMedia(const QString &from, const MediaItem &to);

    // Replace the output settings. Marks the project modified and emits
    // outputChanged when the value actually changes.
    void setOutput(const OutputSettings &output);

    // Step back/forward one settled edit. A no-op with canUndo()/canRedo()
    // false. Rebuilds the whole Timeline (like load()) -- every ClipItem gets
    // a fresh id -- so a caller tracking "the clip under the playhead" by id
    // must rebind after this returns; projectReset() fires for that. Always
    // leaves the project modified (this session's state no longer matches
    // whatever was last saved, even if the content happens to coincide).
    void undo();
    void redo();

signals:
    void modifiedChanged(bool modified);
    void filePathChanged(const QString &path);
    void previewStitchChanged(const stitch::PreviewStitchConfig &config);
    void outputChanged(const OutputSettings &output);
    // Emitted after newProject() / resetWithClip() / load() -- and now
    // undo()/redo() -- have rebuilt the timeline, so a consumer can rebind in
    // one place (the granular Timeline signals fire during the rebuild too).
    void projectReset();
    void canUndoChanged(bool canUndo);
    void canRedoChanged(bool canRedo);

private:
    void onTimelineEdited();
    // Add every timeline source that isn't in the media pool yet. Skipped
    // while rebuilding, unless `rebuilt` (the rebuild's own call at its end).
    void addTimelineSourcesToPool(bool rebuilt = false);
    void setModified(bool modified);
    void setFilePath(const QString &path);

    // Shared guts of save()/load() and the in-memory undo snapshot() /
    // restoreSnapshot(): serialise / parse the whole document (previewStitch
    // + output + every clip). `projectDir` non-null => clip source
    // paths are stored relative-when-inside-it / resolved against it (the
    // file-facing behaviour); null => paths are used as-is, absolute
    // (the in-memory snapshot behaviour). applyDocument rebuilds the
    // Timeline (m_rebuilding-guarded) and emits projectReset(); it does not
    // touch m_filePath/m_modified -- callers own that.
    QByteArray buildDocument(const QDir *projectDir) const;
    bool applyDocument(const QByteArray &text, const QDir *projectDir, QString *error);

    // --- undo/redo (see the .cpp's top-of-file comment) -------------------
    // Connected to every Timeline signal that's a real edit (the same set
    // onTimelineEdited() watches) plus previewStitchChanged/outputChanged. Starts/restarts the coalescing timer; on the
    // first edit of a new burst, snapshots canUndo/canRedo state (redo
    // history dies the moment a new edit happens).
    void onUndoableEdited();
    // Flush a pending burst into m_undoStack (a real undo point) if the live
    // state actually differs from m_lastSettledSnapshot. Called by the
    // coalescing timer and at the top of undo()/redo() so an in-flight edit
    // is never lost to an immediate Ctrl+Z.
    void commitPendingUndoPoint();
    // Current live state, serialised the same way save() does but with
    // absolute paths (no project-directory-relative rewriting -- there may
    // be no saved file yet, and it's irrelevant for an in-memory snapshot).
    QByteArray snapshot() const;
    // Rebuild the Timeline from a snapshot() result. Marks modified; does not
    // touch m_filePath. Clears/reinstates m_rebuilding around the rebuild
    // like load() does.
    void restoreSnapshot(const QByteArray &bytes);
    // Drop all undo/redo history and cancel any pending coalescing timer --
    // called by newProject()/resetWithClip()/load(), which start a fresh
    // editing session that undo shouldn't reach past.
    void clearUndoHistory();

    Timeline *m_timeline = nullptr;
    MediaPool *m_mediaPool = nullptr;
    QString m_filePath;
    stitch::PreviewStitchConfig m_previewStitch;
    OutputSettings m_output;
    bool m_modified = false;
    // True while load()/newProject()/resetWithClip()/undo()/redo() are
    // rebuilding, so the Timeline signals they cause don't set the modified
    // flag / don't recurse into the undo machinery.
    bool m_rebuilding = false;
    std::function<int(const QString &)> m_audioStreamCounter;

    static constexpr int kMaxUndoDepth = 100;
    // Coalescing window: rapid-fire edits (a slider drag, a spinbox held
    // down) within this long of each other collapse into one undo step.
    static constexpr int kUndoCoalesceMs = 600;
    QList<QByteArray> m_undoStack;
    QList<QByteArray> m_redoStack;
    QByteArray m_lastSettledSnapshot;
    bool m_pendingDirty = false;
    QTimer m_undoDebounce;
};
