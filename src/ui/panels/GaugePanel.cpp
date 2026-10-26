#include "GaugePanel.h"

#include "PanelFields.h"

#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

#include <cmath>

using panelfields::numberBox;
using panelfields::sliderRow;

GaugePanel::GaugePanel(QWidget *parent) : SettingsPanel(tr("Gauge"), parent)
{
    m_kind = new QComboBox(this);
    m_kind->setObjectName(QStringLiteral("gaugeKind"));
    m_kind->addItems({tr("Dial"), tr("Bar"), tr("Heading tape")});
    m_kind->setToolTip(tr("A round dial with a needle, a bar that fills up, or a compass strip "
                          "that slides with the direction of travel."));
    form()->addRow(tr("Kind"), m_kind);
    m_reads = new QComboBox(this);
    m_reads->setObjectName(QStringLiteral("gaugeReads"));
    m_reads->addItems({tr("Speed"), tr("Altitude")});
    m_reads->setToolTip(tr("What the gauge shows, from the GPS track of the clip under it. "
                           "Picking another sets the scale to suit it."));
    form()->addRow(tr("Shows"), m_reads);
    m_unit = new QComboBox(this);
    m_unit->setObjectName(QStringLiteral("gaugeUnit"));
    m_unit->setToolTip(tr("The unit of the number and the scale. The scale's ends are "
                          "converted to it."));
    form()->addRow(tr("Unit"), m_unit);
    m_min = numberBox(this, -gauge::Style::kLimit, gauge::Style::kLimit, 0, 10.0, QString());
    m_min->setObjectName(QStringLiteral("gaugeMin"));
    m_min->setToolTip(tr("The scale's low end. A value below it shows there."));
    form()->addRow(tr("Scale from"), m_min);
    m_max = numberBox(this, -gauge::Style::kLimit, gauge::Style::kLimit, 0, 10.0, QString());
    m_max->setObjectName(QStringLiteral("gaugeMax"));
    m_max->setToolTip(tr("The scale's high end. A value above it shows there; the number "
                         "still shows the value."));
    form()->addRow(tr("Scale to"), m_max);
    m_accent = new QPushButton(this);
    m_accent->setObjectName(QStringLiteral("gaugeAccent"));
    m_accent->setToolTip(tr("The needle, the bar, the tape's marker."));
    form()->addRow(tr("Accent"), m_accent);
    m_face = new QPushButton(this);
    m_face->setObjectName(QStringLiteral("gaugeFace"));
    m_face->setToolTip(tr("The colour behind the gauge."));
    form()->addRow(tr("Face"), m_face);
    form()->addRow(tr("Face opacity"),
                   sliderRow(this, 0, 100, &m_faceOpacity, &m_faceOpacityValue));
    m_faceOpacity->setObjectName(QStringLiteral("gaugeFaceOpacity"));
    m_faceOpacity->setToolTip(tr("0% leaves only the scale and the numbers over the picture."));
    m_text = new QPushButton(this);
    m_text->setObjectName(QStringLiteral("gaugeText"));
    m_text->setToolTip(tr("The scale and the numbers."));
    form()->addRow(tr("Text"), m_text);
    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("gaugeNote"));
    m_note->setWordWrap(true);
    m_note->setVisible(false);
    form()->addRow(m_note);

    auto edited = [this] {
        if (m_updating)
            return;
        m_style = current();
        emit gaugeEdited(m_style);
    };
    connect(m_kind, &QComboBox::activated, this, [this, edited] {
        showRows();
        edited();
    });
    // Another value or unit: a scale that suits it.
    connect(m_reads, &QComboBox::activated, this, [this, edited] {
        m_style.reads = static_cast<gauge::Reads>(m_reads->currentIndex());
        const gauge::Range r = gauge::defaultRange(m_style.reads, m_style.units);
        m_updating = true;
        showRows();
        m_min->setValue(r.min);
        m_max->setValue(r.max);
        m_updating = false;
        edited();
    });
    connect(m_unit, &QComboBox::activated, this, [this, edited](int index) {
        double factor = 1.0;
        if (m_style.reads == gauge::Reads::Speed) {
            const auto unit = static_cast<title::SpeedUnit>(index);
            if (unit != m_style.units.speed)
                factor = unit == title::SpeedUnit::Mph ? 1.0 / 1.609344 : 1.609344;
            m_style.units.speed = unit;
        } else {
            const auto unit = static_cast<title::AltitudeUnit>(index);
            if (unit != m_style.units.altitude)
                factor = unit == title::AltitudeUnit::Feet ? 1.0 / 0.3048 : 0.3048;
            m_style.units.altitude = unit;
        }
        m_updating = true;
        m_min->setValue(std::round(m_min->value() * factor));
        m_max->setValue(std::round(m_max->value() * factor));
        m_updating = false;
        edited();
    });
    connect(m_min, &QDoubleSpinBox::valueChanged, this, edited);
    connect(m_max, &QDoubleSpinBox::valueChanged, this, edited);
    connect(m_faceOpacity, &QSlider::valueChanged, this, [this, edited](int v) {
        m_faceOpacityValue->setText(tr("%1%").arg(v));
        edited();
    });
    const auto colorButton = [this, edited](QPushButton *b, uint32_t gauge::Style::*field,
                                            const QString &title) {
        connect(b, &QPushButton::clicked, this, [this, b, field, title, edited] {
            const QColor c = QColorDialog::getColor(QColor::fromRgb(m_style.*field), this, title);
            if (!c.isValid())
                return;
            m_style.*field = c.rgb() & 0xffffff;
            setColor(b, m_style.*field);
            edited();
        });
    };
    colorButton(m_accent, &gauge::Style::accent, tr("Accent colour"));
    colorButton(m_face, &gauge::Style::face, tr("Face colour"));
    colorButton(m_text, &gauge::Style::text, tr("Text colour"));
    setGauge({});
}

gauge::Style GaugePanel::current() const
{
    gauge::Style s = m_style;
    s.kind = static_cast<gauge::Kind>(std::max(0, m_kind->currentIndex()));
    s.reads = static_cast<gauge::Reads>(std::max(0, m_reads->currentIndex()));
    s.min = m_min->value();
    s.max = m_max->value();
    s.faceOpacity = float(m_faceOpacity->value() / 100.0);
    return s;
}

void GaugePanel::setColor(QPushButton *button, uint32_t rgb)
{
    panelfields::showColor(button, rgb);
}

void GaugePanel::showRows()
{
    const bool scale = m_kind->currentIndex() != static_cast<int>(gauge::Kind::Tape);
    for (QWidget *w : {static_cast<QWidget *>(m_reads), static_cast<QWidget *>(m_unit),
                       static_cast<QWidget *>(m_min), static_cast<QWidget *>(m_max)})
        form()->setRowVisible(w, scale);
    const bool speed = m_reads->currentIndex() == static_cast<int>(gauge::Reads::Speed);
    const QSignalBlocker block(m_unit);
    m_unit->clear();
    if (speed) {
        m_unit->addItems({tr("km/h"), tr("mph")});
        m_unit->setCurrentIndex(static_cast<int>(m_style.units.speed));
    } else {
        m_unit->addItems({tr("metres"), tr("feet")});
        m_unit->setCurrentIndex(static_cast<int>(m_style.units.altitude));
    }
}

void GaugePanel::setGauge(const gauge::Style &s, const QString &note)
{
    m_updating = true;
    m_style = s;
    m_kind->setCurrentIndex(static_cast<int>(s.kind));
    m_reads->setCurrentIndex(static_cast<int>(s.reads));
    showRows();
    m_min->setValue(s.min);
    m_max->setValue(s.max);
    setColor(m_accent, s.accent);
    setColor(m_face, s.face);
    m_faceOpacity->setValue(int(std::lround(s.faceOpacity * 100.0f)));
    m_faceOpacityValue->setText(tr("%1%").arg(m_faceOpacity->value()));
    setColor(m_text, s.text);
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());
    setHint(tr("The gauge selected on the timeline or in the view. Its value comes from the GPS "
               "track of the clip under it."));
    m_updating = false;
}
