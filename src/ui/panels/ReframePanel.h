#pragma once

#include "../SettingsPanel.h"

#include "../../core/PaneLayout.h"   // pane::Split
#include "../../core/ReframeTrack.h" // ReframePose

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

// Numeric yaw / pitch / roll / field-of-view fields for the view orientation,
// an alternative to drag-authoring in the viewport. An edit sets the live
// viewport pose immediately; when reframe authoring is armed (the transport
// bar's Reframe toggle) it also writes a keyframe at the playhead, exactly
// like a viewport drag commit. The fields reflect the current pose as playback
// / scrubbing moves.
//
// All fields are shown in degrees; the signal / setter use radians to match
// ReframePose.
//
// Split screen: the clip's layout (one picture, side by side, stacked, a
// 2 x 2 grid), the divider between the panes, which pane the fields and the
// viewport steer (the active pane) and Fill panes, which points the panes
// front / back / left / right in one go.
class ReframePanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit ReframePanel(QWidget *parent = nullptr);

    // Reflect the current pose (from the reframe path or a viewport drag).
    // Does not emit poseEdited.
    void setPose(const ReframePose &pose);

    // Show a warning that the clip under the playhead has no reframe keys
    // (so it exports a static forward view, whatever the viewport shows).
    void setClipUnkeyed(bool unkeyed);

    // "Track subject…": `reason` non-empty when it can't run here (no
    // tracking in this build, or its model is missing) -- the button stays
    // disabled and `reason` shows under it. `selecting` shows it checked
    // while a box is being drawn.
    void setTrackUnavailable(const QString &reason);
    void setTrackEnabled(bool enabled);
    void setTrackSelecting(bool selecting);
    // While tracking runs: the button reads "Stop tracking" (the one control
    // that stays live -- trackStopButton()) and `status` shows under it.
    void setTrackRunning(bool running);
    void setTrackStatus(const QString &status);
    QWidget *trackStopButton() const;

    // A flat video (ClipItem::flat) isn't reframed: the view fields and
    // Track subject give way to how it fills the output frame -- the whole
    // picture with bars, or filling the frame (`fill`).
    void setFlatClip(bool flat, bool fill = false);

    // Easing of the move from the key at (or before) the playhead to the next
    // one; `available` false greys it out (the playhead isn't between two
    // keys). Does not emit easeEdited.
    void setEase(bool available, keytrack::Ease ease);

    // The clip's split and the active pane (0-based). Do not emit.
    void setSplit(const pane::Split &split);
    void setActivePane(int pane);
signals:
    // The layout or the divider changed.
    void splitEdited(const pane::Split &split);
    // Another pane picked to steer.
    void activePaneEdited(int pane);
    void fillPanesRequested();
    void poseEdited(const ReframePose &pose);
    void easeEdited(keytrack::Ease ease);
    // The Fit choice of a flat clip changed.
    void fitEdited(bool fill);
    // The button was clicked: start drawing a box around the subject (or,
    // while selecting, stop).
    void trackSubjectToggled(bool start);
    void trackStopRequested();

private:
    ReframePose currentPose() const;
    void emitIfEdited();
    pane::Split currentSplit() const;
    void showSplitRows();
    void setGapColor(uint32_t rgb);

    QDoubleSpinBox *m_yaw = nullptr;
    QDoubleSpinBox *m_pitch = nullptr;
    QDoubleSpinBox *m_roll = nullptr;
    QDoubleSpinBox *m_fov = nullptr;
    QComboBox *m_ease = nullptr;
    bool m_easeAvailable = false;
    QPushButton *m_track = nullptr;
    QLabel *m_trackUnavailable = nullptr;
    QString m_trackUnavailableReason;
    QLabel *m_trackStatus = nullptr;
    QLabel *m_unkeyed = nullptr;
    QComboBox *m_fit = nullptr;
    QComboBox *m_layout = nullptr;
    QComboBox *m_pane = nullptr;
    QDoubleSpinBox *m_gap = nullptr;
    QPushButton *m_gapColor = nullptr;
    QPushButton *m_fillPanes = nullptr;
    uint32_t m_gapRgb = 0;
    bool m_flat = false;
    bool m_trackRunning = false;
    bool m_updating = false; // true while setPose() writes the widgets
};
