#pragma once

#include "../SettingsPanel.h"

#include "../../core/ClipEffects.h"

class QCheckBox;
class QLabel;
class QSlider;
class QToolButton;
class QWidget;

// The Video tab's Effects section: the video effects on the clip under the
// playhead (the selected one), each with its own rows -- on / off, Remove,
// and its settings -- in the order they run. Effects are put on from the
// left sidebar's Effects tab. An edit hands the clip's new fx::ClipEffects
// back to MainWindow, which writes it onto the clip (Timeline::setClipEffects).
class ClipEffectsPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit ClipEffectsPanel(QWidget *parent = nullptr);

    // Reflect a clip's effects. Does not emit effectsEdited.
    void setState(const fx::ClipEffects &effects);
    // No clip under the playhead: none, greyed out. Does not emit.
    void setNoClip();

signals:
    void effectsEdited(const fx::ClipEffects &effects);

private:
    fx::ClipEffects currentEffects() const;
    void emitIfEdited();

    fx::ClipEffects m_shown;
    QLabel *m_none = nullptr; // "No effects": how to put one on
    // Vignette.
    QWidget *m_vignetteHead = nullptr;
    QCheckBox *m_vignetteOn = nullptr;
    QToolButton *m_vignetteRemove = nullptr;
    QWidget *m_vignetteAmountRow = nullptr;
    QSlider *m_vignetteAmount = nullptr;
    QLabel *m_vignetteAmountValue = nullptr;
    QWidget *m_vignetteSizeRow = nullptr;
    QSlider *m_vignetteSize = nullptr;
    QLabel *m_vignetteSizeValue = nullptr;
    QWidget *m_vignetteSoftnessRow = nullptr;
    QSlider *m_vignetteSoftness = nullptr;
    QLabel *m_vignetteSoftnessValue = nullptr;
    bool m_updating = false;
};
