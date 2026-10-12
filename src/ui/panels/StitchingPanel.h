#pragma once

#include "../SettingsPanel.h"

#include <IStitchSource.h> // stitch::PreviewStitchConfig

class QCheckBox;
class QComboBox;
class QLabel;

// The stitch settings that change the output: lens guards (selects the
// trailer's lens profile) and the depth-aware seam, plus a line about the
// clip's seam alignment. The preview-only settings of the same config
// (quality, preview size) are in the View menu; setConfig() keeps them so an
// edit here hands them back unchanged. Edits emit configChanged; MainWindow
// routes that into Project::setPreviewStitch (persisted on the .orp) and
// PlaybackController::setPreviewConfig (re-stitches the live preview).
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

    stitch::PreviewStitchConfig m_config; // the last one set: the View menu's part
    QComboBox *m_lensGuard = nullptr;
    QCheckBox *m_depthSeam = nullptr;
    QLabel *m_seamStatus = nullptr;
    bool m_updating = false; // true while setConfig() is writing the widgets
};
