#pragma once

#include "../SettingsPanel.h"

#include <IStitchSource.h> // stitch::PreviewStitchConfig::LensGuard, Method

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

// The stitch settings of the clip it shows (the selected one, else the one
// under the playhead), which change the output: lens guards (selects the
// trailer's lens profile) and the stitcher (stitch::PreviewStitchConfig::
// Method), plus a line about the clip's seam alignment. They belong to the clip's file, so every clip cut
// from it shares them (Timeline::setSourceStitch). The preview-only
// settings (quality, preview size) are project-wide, in the View menu.
// Edits emit stitchEdited; MainWindow writes them onto the clip's file. Lens
// guards the file has no profile for can't be picked; a file with none (the
// ONE X2, the X6) shows None, greyed out, with a line saying why. Lens
// calibration: the camera file's, or one made with Calibrate lenses (the
// choices MainWindow offers for the clip, setCalibrations).
class StitchingPanel : public SettingsPanel {
    Q_OBJECT

public:
    using LensGuard = stitch::PreviewStitchConfig::LensGuard;
    using Method = stitch::PreviewStitchConfig::Method;

    explicit StitchingPanel(QWidget *parent = nullptr);

    // Show a clip's settings, `lensGuard` as the stitch applies it to a file
    // carrying `profiles`. Does not emit.
    void setState(LensGuard lensGuard, Method method,
                  stitch::LensGuardProfiles profiles = stitch::LensGuardProfiles::all());
    // No clip to show: a new clip's settings, greyed out (Calibrate lenses
    // stays: it needs no clip). Does not emit.
    void setNoClip();
    // The clip's seam colour blend (ClipItem::seamColourBlend), offered
    // only with the new stitcher. Does not emit.
    void setSeamColourBlend(bool on);
    // ... and its seam flow (ClipItem::seamFlow), likewise. Does not emit.
    void setSeamFlow(bool on);
    // One line about the current clip's seam alignment and lens colour
    // (empty hides it); `details`, the measured figures, go in its tooltip.
    void setSeamStatus(const QString &text, const QString &details = QString());
    // The lens calibrations the clip can be stitched with: `labels[0]` is the
    // camera file's, the others calibrations made with Calibrate lenses
    // (`tooltips` their figures); `current` the one it has. Does not emit.
    void setCalibrations(const QStringList &labels, const QStringList &tooltips, int current);
    // Whether "Calibrate lenses…" can run; `whyNot` its tooltip when not.
    void setCalibrateAvailable(bool available, const QString &whyNot = QString());

signals:
    void stitchEdited(LensGuard lensGuard, Method method);
    void seamColourBlendEdited(bool on);
    void seamFlowEdited(bool on);
    // "Apply to all clips": give every 360 clip the settings shown.
    void applyToAllRequested();
    // A lens calibration picked (an index into setCalibrations' labels).
    void calibrationChosen(int index);
    // "Calibrate lenses…".
    void calibrateRequested();

private:
    void emitIfEdited();
    // The new stitcher's own options, enabled with it.
    void updateNewStitcherOptions();

    QComboBox *m_lensGuard = nullptr;
    QLabel *m_noGuards = nullptr;
    QComboBox *m_method = nullptr;
    QCheckBox *m_seamColourBlend = nullptr;
    QCheckBox *m_seamFlow = nullptr;
    QComboBox *m_calibration = nullptr;
    QPushButton *m_calibrate = nullptr;
    QLabel *m_seamStatus = nullptr;
    QPushButton *m_applyToAll = nullptr;
    bool m_updating = false; // true while setState() is writing the widgets
};
