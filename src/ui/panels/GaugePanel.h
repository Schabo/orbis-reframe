#pragma once

#include "../SettingsPanel.h"

#include "../../core/Gauge.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;

// The Title tab's Gauge section, in the place of Text and Look while a gauge
// is selected: what kind of gauge it is, what it shows and in which unit,
// its scale, and its colours (gauge::Style). Where it sits, its size and
// timing are the tab's last section; how it comes and goes, the Animation
// section, as a title's.
//
// Pure view like the other panels: edits go back to MainWindow as signals.
class GaugePanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit GaugePanel(QWidget *parent = nullptr);

    // Reflect a gauge. Does not emit. `note`, shown under the fields when
    // not empty: where it has no GPS or motion to show.
    void setGauge(const gauge::Style &style, const QString &note = QString());

signals:
    // The whole style: the last one set with this section's fields changed.
    void gaugeEdited(const gauge::Style &style);

private:
    gauge::Style current() const;
    // Show the rows the kind uses; the unit list for what it shows.
    void showRows();
    void setColor(QPushButton *button, uint32_t rgb);

    QComboBox *m_kind = nullptr;  // gauge::Kind order
    QComboBox *m_reads = nullptr; // gauge::Reads order
    QComboBox *m_unit = nullptr;  // title::SpeedUnit or AltitudeUnit order
    QDoubleSpinBox *m_min = nullptr;
    QDoubleSpinBox *m_max = nullptr;
    QPushButton *m_accent = nullptr;
    QPushButton *m_face = nullptr;
    QSlider *m_faceOpacity = nullptr;
    QLabel *m_faceOpacityValue = nullptr;
    QPushButton *m_text = nullptr;
    QLabel *m_note = nullptr;
    gauge::Style m_style; // as last set, then as edited
    bool m_updating = false;
};
