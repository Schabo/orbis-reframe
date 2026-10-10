#pragma once

#include "../SettingsPanel.h"

#include "../../core/ClipItem.h" // ColorGrade

class QLabel;
class QPushButton;
class QSlider;

// Per-clip colour grade: Exposure / Contrast / Saturation / Temperature
// sliders for the clip currently under the playhead. Mirrors the Audio
// panel's pattern -- an edit hands the new ColorGrade back to MainWindow to
// write onto that clip via Timeline::setClipColor; nothing here talks to the
// renderer directly. Applied at render time in both playback and export --
// see ColorGrade's own comment for where. Reset / Copy / Paste ask
// MainWindow, which holds the copied grade (shared with the timeline's
// right-click menu).
class ColourPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit ColourPanel(QWidget *parent = nullptr);

    // Reflect a clip's grade. Does not emit gradeEdited.
    void setState(const ColorGrade &grade);
    // No clip under the playhead (e.g. an empty timeline) -- greys the panel out.
    void setNoClip();
    // Whether a grade has been copied (enables Paste).
    void setCanPaste(bool canPaste);

signals:
    void gradeEdited(const ColorGrade &grade);
    void copyRequested();
    void pasteRequested();

private:
    ColorGrade currentGrade() const;
    void emitIfEdited();

    QSlider *m_exposure = nullptr; // tenths of a stop: -30..30 -> -3.0..3.0 EV
    QLabel *m_exposureValue = nullptr;
    QSlider *m_contrast = nullptr; // percent: -100..100
    QLabel *m_contrastValue = nullptr;
    QSlider *m_saturation = nullptr; // percent: -100..100
    QLabel *m_saturationValue = nullptr;
    QSlider *m_temperature = nullptr; // percent: -100..100
    QLabel *m_temperatureValue = nullptr;
    QPushButton *m_reset = nullptr;
    QPushButton *m_paste = nullptr;
    bool m_updating = false;
};
