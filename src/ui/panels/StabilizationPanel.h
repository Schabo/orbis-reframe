#pragma once

#include "../SettingsPanel.h"

#include <GyroStabilizer.h> // insv::StabConfig

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QWidget;

// Stabilization mode picker + per-mode controls for one clip (the selected
// one, else the one under the playhead), like the Colour panel. All modes
// drive the native reprojection-time path (insv::GyroStabilizer,
// ClipItem::stabilization):
//   Off;
//   Lean follow  -- smooth the shake, keep the bike's lean; one Strength slider
//                   retunes its shared smoothing window;
//   Horizon Lock -- accel-referenced level horizon (helmet / boat / handheld /
//                   bump tilt); one Amount slider (0 = off .. 1 = fully level);
//   Custom       -- the raw per-axis windows + level-horizon + mount offsets.
// MainWindow writes the emitted insv::StabConfig onto the clip
// (Timeline::setClipStabilization). Copy / Paste ask MainWindow, which holds
// the copied setting (shared with the timeline's right-click menu).
class StabilizationPanel : public SettingsPanel {
    Q_OBJECT

public:
    enum class Mode { Off, LeanFollow, HorizonLock, Custom };

    explicit StabilizationPanel(QWidget *parent = nullptr);

    // Reflect a clip's setting. Does not emit.
    // `cfg` is the native gyro config for `mode`: for Lean follow the Strength
    // slider is derived from its shared window, for Custom it fills the raw
    // knobs, otherwise it is ignored.
    void setState(Mode mode, const insv::StabConfig &cfg);

    // One line about the active clip's video<->gyro sync fine-tune (measured
    // in the background). Empty hides it.
    void setSyncStatus(const QString &text);
    // No clip to show (e.g. an empty timeline): a new clip's setting (Off),
    // greyed out. Does not emit.
    void setNoClip();
    // Whether a setting has been copied (enables Paste).
    void setCanPaste(bool canPaste);

signals:
    // mode is the picker choice; nativeConfig is the insv::StabConfig for that
    // mode (LeanFollow window from the Strength slider, Custom from the raw
    // knobs, an Off config otherwise).
    void modeChanged(StabilizationPanel::Mode mode, const insv::StabConfig &nativeConfig);
    void copyRequested();
    void pasteRequested();

private:
    void emitIfEdited();
    insv::StabConfig nativeConfigForMode() const;
    void updateModeVisibility();

    QComboBox *m_mode = nullptr;
    QWidget *m_leanRow = nullptr; // shown only for Lean follow
    QSlider *m_leanStrength = nullptr;    // 0..100 %, maps to the shared window
    QLabel *m_leanStrengthValue = nullptr;
    QWidget *m_horizonRow = nullptr; // shown only for Horizon Lock
    QSlider *m_horizonAmount = nullptr;   // 0..100 %, maps to horizonLockAmount
    QLabel *m_horizonAmountValue = nullptr;
    QWidget *m_horizonSmoothRow = nullptr; // shown only for Horizon Lock
    QSlider *m_horizonSmooth = nullptr;    // 0..100 %, maps to the heading window
    QLabel *m_horizonSmoothValue = nullptr;
    QDoubleSpinBox *m_winYaw = nullptr;
    QDoubleSpinBox *m_winPitch = nullptr;
    QDoubleSpinBox *m_winRoll = nullptr;
    QDoubleSpinBox *m_lockAmount = nullptr;
    QDoubleSpinBox *m_pitchOffset = nullptr;
    QDoubleSpinBox *m_rollOffset = nullptr;
    QCheckBox *m_autoLevel = nullptr; // Custom: StabConfig::autoLevel
    QWidget *m_customRows = nullptr; // shown only for Custom
    QWidget *m_accelLpRow = nullptr; // accel low-pass; shown for Horizon lock / Custom
    QDoubleSpinBox *m_accelLowPass = nullptr; // accel fusion low-pass cutoff, Hz (0 = off)
    QLabel *m_syncStatus = nullptr;
    QPushButton *m_paste = nullptr;
    bool m_updating = false;
};
