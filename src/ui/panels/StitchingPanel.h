#pragma once

#include "../SettingsPanel.h"

#include <IStitchSource.h> // stitch::PreviewStitchConfig::LensGuard

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

// The stitch settings of the clip it shows (the selected one, else the one
// under the playhead), which change the output: lens guards (selects the
// trailer's lens profile) and the depth-aware seam, plus a line about the
// clip's seam alignment. They belong to the clip's file, so every clip cut
// from it shares them (Timeline::setSourceStitch). The preview-only
// settings (quality, preview size) are project-wide, in the View menu.
// Edits emit stitchEdited; MainWindow writes them onto the clip's file. Lens
// guards the file has no profile for can't be picked; a file with none (the
// ONE X2, the X6) shows None, greyed out, with a line saying why.
class StitchingPanel : public SettingsPanel {
    Q_OBJECT

public:
    using LensGuard = stitch::PreviewStitchConfig::LensGuard;

    explicit StitchingPanel(QWidget *parent = nullptr);

    // Show a clip's settings, `lensGuard` as the stitch applies it to a file
    // carrying `profiles`. Does not emit.
    void setState(LensGuard lensGuard, bool depthSeam,
                  stitch::LensGuardProfiles profiles = stitch::LensGuardProfiles::all());
    // No clip to show: a new clip's settings, greyed out. Does not emit.
    void setNoClip();
    // One line about the current clip's seam alignment (empty hides it).
    void setSeamStatus(const QString &text);

signals:
    void stitchEdited(LensGuard lensGuard, bool depthSeam);
    // "Apply to all clips": give every 360 clip the settings shown.
    void applyToAllRequested();

private:
    void emitIfEdited();

    QComboBox *m_lensGuard = nullptr;
    QLabel *m_noGuards = nullptr;
    QCheckBox *m_depthSeam = nullptr;
    QLabel *m_seamStatus = nullptr;
    QPushButton *m_applyToAll = nullptr;
    bool m_updating = false; // true while setState() is writing the widgets
};
