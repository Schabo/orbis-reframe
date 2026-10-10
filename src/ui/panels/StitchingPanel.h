#pragma once

#include "../SettingsPanel.h"

#include <IStitchSource.h> // stitch::PreviewStitchConfig

class QCheckBox;
class QComboBox;
class QLabel;

// Preview-stitch settings: quality (shown as "Standard" = Quality::Template /
// Optical flow), preview equirect resolution (2:1 presets only -- the
// reprojection shader assumes equirect),
// lens guards (selects the trailer's lens profile; also used by export) and
// the depth-aware seam (also used by export). Edits emit configChanged;
// MainWindow routes that into Project::setPreviewStitch (persisted on the
// .orp) and PlaybackController::setPreviewConfig (re-stitches the live
// preview under a new cache tag).
class StitchingPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit StitchingPanel(QWidget *parent = nullptr);

    // Reflect a config set elsewhere (project load / reset). Does not emit.
    void setConfig(const stitch::PreviewStitchConfig &config);
    // One line about the current clip's seam alignment (empty hides it).
    void setSeamStatus(const QString &text);

signals:
    void configChanged(const stitch::PreviewStitchConfig &config);

private:
    stitch::PreviewStitchConfig currentConfig() const;
    void emitIfEdited();

    QComboBox *m_quality = nullptr;
    QComboBox *m_resolution = nullptr;
    QComboBox *m_lensGuard = nullptr;
    QCheckBox *m_depthSeam = nullptr;
    QLabel *m_seamStatus = nullptr;
    bool m_updating = false; // true while setConfig() is writing the widgets
};
