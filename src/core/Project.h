#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <GyroStabilizer.h> // insv::StabConfig (POD; no FFmpeg/Qt)
#include <IStitchSource.h>  // stitch::PreviewStitchConfig (POD; no FFmpeg)

#include "OutputSettings.h"

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

    // The file format. Only kFormatVersion loads: older and newer files are
    // refused. A field may be added without a bump when absent => today's
    // behaviour; anything an older build would misread bumps the version.
    // Optional fields are written only when set.
    //   "format": kFormatTag, "version": kFormatVersion,
    //   "previewStitch": {quality: "template" | "optflow", width, height
    //     (0 = auto), lensGuard: "none" | "standard" | "premium", depthSeam},
    //   "stabilization": {mode, windowYawSec, windowPitchSec, windowRollSec,
    //     horizonLockAmount, horizonGain, accelLowPassHz, pitchOffsetRad,
    //     rollOffsetRad, autoLevel (absent => true)},
    //   "output": {aspect: "16:9" | "9:16" | "1:1" | "4:3", width, height,
    //     fps (0 = match the first clip)},
    //   "timeline": {clips: [...], mainMuted, tracks: [...]}.
    // A main-track clip: source, trimInMs, trimOutMs, sourceDurationMs, fps,
    //   sourceWidth, sourceHeight, reframe {keys: [{tMs, yaw, pitch, roll,
    //   fovY}]}, audioGainDb, audioMuted, audioTrack, color {exposureEv,
    //   contrast, saturation, temperature}, speed; optional: noise {mode:
    //   "wind" (motorcycle) | "windGeneral" | "hiss" | "voice" |
    //   "voiceAmbience", strength}, syncAdjustMs and seamAdjust {deg: [yaw,
    //   pitch, roll], lensGuard} once measured, and privacy:
    //     "plates": true -- the blur is on;
    //     "removed" -- blur tracks removed by hand, [{fromMs, toMs,
    //       wholeTrack, path: [[tMs, x, y, z], ...]}] (privacy::BlurRemoval);
    //     "added" -- plates blurred by hand, [{frames: [[tMs, kind, 12 corner
    //       coordinates in millionths], ...], timing: "corrected" |
    //       "uncorrected" | "noGyro"}] (privacy::BlurAddition).
    // A track: {kind: "audio" | "video", name, gainDb, muted, clips}.
    //   An audio clip: source, stream, sourceDurationMs, startMs, trimInMs,
    //   trimOutMs, speed, gainDb, muted, fadeInMs, fadeOutMs; optional: noise,
    //   and when linked "linkedClip" (main-track index) or "linkedOverlay"
    //   [track index, clip index], + syncOffsetMs.
    //   A video (overlay) clip: the audio clip's timing fields + sourceWidth,
    //   sourceHeight, inset {centerX, centerY, width, opacity, cornerRadius,
    //   borderWidth, borderColor "#rrggbb"}, insetKeys [{tMs, centerX,
    //   centerY, width, opacity}].
    static constexpr int kFormatVersion = 8;
    static constexpr const char *kFormatTag = "orbis-reframe-project";
    static constexpr const char *kFileExtension = "orp"; // no dot

    Timeline *timeline() const { return m_timeline; }
    // Empty until the project is saved or loaded.
    QString filePath() const { return m_filePath; }
    bool modified() const { return m_modified; }

    // Project-wide preview-stitch settings (Stitching panel). The live
    // playback path is handed this via PlaybackController::setPreviewConfig.
    const stitch::PreviewStitchConfig &previewStitch() const { return m_previewStitch; }

    // Project-wide native gyro stabilization (Stabilization panel: Off / Lean
    // follow / Horizon Lock / Custom). Applied at reprojection time (composed
    // with the reframe pose) -- not a stitch setting, so a change never
    // re-stitches.
    const insv::StabConfig &stabilization() const { return m_stabilization; }

    // Project-wide output format (Project Settings panel): the aspect the
    // viewport is pinned to, the default export size, and the export rate.
    const OutputSettings &output() const { return m_output; }

    // Undo/redo (whole-project snapshots -- see the .cpp's top-of-file
    // comment for why and how edits are coalesced into one step per
    // "settled" user action).
    bool canUndo() const { return m_pendingDirty || !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

public slots:
    // Empty timeline, no file, not modified.
    void newProject();

    // Replace the timeline with a single loose clip -- opening a bare .insv,
    // not a project file. Clears the file path and the modified flag. Returns
    // the new clip's id.
    int resetWithClip(const QString &sourcePath, qint64 durationMs, double fps,
                      int width = 0, int height = 0);

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

    // Replace the stabilization settings. Marks the project modified and emits
    // stabilizationChanged when the value actually changes.
    void setStabilization(const insv::StabConfig &config);

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
    void stabilizationChanged(const insv::StabConfig &config);
    void outputChanged(const OutputSettings &output);
    // Emitted after newProject() / resetWithClip() / load() -- and now
    // undo()/redo() -- have rebuilt the timeline, so a consumer can rebind in
    // one place (the granular Timeline signals fire during the rebuild too).
    void projectReset();
    void canUndoChanged(bool canUndo);
    void canRedoChanged(bool canRedo);

private:
    void onTimelineEdited();
    void setModified(bool modified);
    void setFilePath(const QString &path);

    // Shared guts of save()/load() and the in-memory undo snapshot() /
    // restoreSnapshot(): serialise / parse the whole document (previewStitch
    // + stabilization + output + every clip). `projectDir` non-null => clip source
    // paths are stored relative-when-inside-it / resolved against it (the
    // file-facing behaviour); null => paths are used as-is, absolute
    // (the in-memory snapshot behaviour). applyDocument rebuilds the
    // Timeline (m_rebuilding-guarded) and emits projectReset(); it does not
    // touch m_filePath/m_modified -- callers own that.
    QByteArray buildDocument(const QDir *projectDir) const;
    bool applyDocument(const QByteArray &text, const QDir *projectDir, QString *error);

    // --- undo/redo (see the .cpp's top-of-file comment) -------------------
    // Connected to every Timeline signal that's a real edit (the same set
    // onTimelineEdited() watches) plus previewStitchChanged/
    // stabilizationChanged/outputChanged. Starts/restarts the coalescing timer; on the
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
    QString m_filePath;
    stitch::PreviewStitchConfig m_previewStitch;
    insv::StabConfig m_stabilization;
    OutputSettings m_output;
    bool m_modified = false;
    // True while load()/newProject()/resetWithClip()/undo()/redo() are
    // rebuilding, so the Timeline signals they cause don't set the modified
    // flag / don't recurse into the undo machinery.
    bool m_rebuilding = false;

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
