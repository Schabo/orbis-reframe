#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <atomic>

class QThread;
class Project;
struct ClipItem;

// The per-source measurements run in the background: the gyro sync
// fine-tune (stitch::MeasureClipSync, only with ORBIS_REFRAME_HAVE_CLIP_SYNC)
// and the seam alignment (stitch::MeasureClipSeam), one of each kind at a
// time, for the timeline's sources in order. A result lands on the Timeline
// (setSourceSyncAdjust / setSourceSeamAdjust), whose clipSyncChanged /
// clipSeamChanged say what to rebuild.
//
// They run when a clip is added or a project opened, never while playing:
// a measurement competes with playback for the GPU and shows as a hitch.
// One running when play starts is cancelled (it would stall playback its
// whole run) and starts over on the pause.
class ClipMeasurements : public QObject {
    Q_OBJECT

public:
    // `enabled` false (tests): nothing is ever measured.
    ClipMeasurements(Project *project, bool enabled, QObject *parent = nullptr);
    // Cancels a running measurement and waits for it.
    ~ClipMeasurements() override;

    // Start whichever measurement is due next. Runs after the current event
    // (a load or add finishes first); not while playing.
    void schedule();
    // Playing: cancel the running ones. Paused: schedule() again.
    void setPlaying(bool playing);
    // A source's lens-guard setting changed: its seam measurement, if it
    // failed, may work under the new one.
    void forgetSeamFailure(const QString &clipPath) { m_seamFailed.remove(clipPath); }

    // The Stabilization / Stitching panels' status lines for clip `c` (null =
    // none): measured, measuring, failed, or empty.
    QString syncStatus(const ClipItem *c) const;
    QString seamStatus(const ClipItem *c) const;

signals:
    // A measurement started or finished: the status lines changed.
    void statusChanged();

private:
    void maybeMeasureSync(const QString &clipPath);
    void maybeMeasureSeam(const QString &clipPath);
    // The first main-track clip cut from `clipPath`, if its file is there.
    const ClipItem *measurableClip(const QString &clipPath) const;

    Project *m_project;
    bool m_enabled;
    bool m_playing = false;
    bool m_queued = false; // schedule() already pending

    QThread *m_syncThread = nullptr;
    QString m_syncMeasuringPath;
    std::atomic<bool> m_syncCancel{false};
    // Sources that gave no result this session, with why, for the panel:
    // each its own, two clips can fail differently.
    QHash<QString, QString> m_syncFailed;

    QThread *m_seamThread = nullptr;
    QString m_seamMeasuringPath;
    std::atomic<bool> m_seamCancel{false};
    QHash<QString, QString> m_seamFailed; // as m_syncFailed
};
