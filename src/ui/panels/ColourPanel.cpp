#include "ColourPanel.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

// The exposure slider counts tenths of a stop; the others whole percent.
constexpr float kExposureStepsPerEv = 10.0f;
constexpr int kMinExposureSteps = int(ColorGrade::kMinExposureEv * kExposureStepsPerEv);
constexpr int kMaxExposureSteps = int(ColorGrade::kMaxExposureEv * kExposureStepsPerEv);
constexpr int kMinPercent = int(ColorGrade::kMinPercent);
constexpr int kMaxPercent = int(ColorGrade::kMaxPercent);

// A slider + live value label in one row, the shape every row here shares
// (matches StabilizationPanel's Strength/Amount rows).
struct SliderRow {
    QWidget *row = nullptr;
    QSlider *slider = nullptr;
    QLabel *value = nullptr;
};

SliderRow makeSliderRow(QWidget *parent, int lo, int hi, int step, int page)
{
    SliderRow r;
    r.row = new QWidget(parent);
    auto *layout = new QHBoxLayout(r.row);
    layout->setContentsMargins(0, 0, 0, 0);
    r.slider = new QSlider(Qt::Horizontal, r.row);
    r.slider->setRange(lo, hi);
    r.slider->setSingleStep(step);
    r.slider->setPageStep(page);
    r.slider->setTickPosition(QSlider::TicksBelow);
    r.slider->setTickInterval((hi - lo) / 4);
    r.value = new QLabel(r.row);
    r.value->setMinimumWidth(r.row->fontMetrics().horizontalAdvance(QStringLiteral("-100%")));
    layout->addWidget(r.slider, 1);
    layout->addWidget(r.value);
    return r;
}

} // namespace

ColourPanel::ColourPanel(QWidget *parent)
    : SettingsPanel(tr("Colour"), QStringLiteral("palette"), parent)
{
    SliderRow exposure = makeSliderRow(this, kMinExposureSteps, kMaxExposureSteps, 1, 5);
    m_exposure = exposure.slider;
    m_exposureValue = exposure.value;
    m_exposure->setToolTip(tr("Brighter or darker, in stops: +1 doubles the light."));
    form()->addRow(tr("Exposure"), exposure.row);

    SliderRow contrast = makeSliderRow(this, kMinPercent, kMaxPercent, 1, 10);
    m_contrast = contrast.slider;
    m_contrastValue = contrast.value;
    form()->addRow(tr("Contrast"), contrast.row);

    SliderRow saturation = makeSliderRow(this, kMinPercent, kMaxPercent, 1, 10);
    m_saturation = saturation.slider;
    m_saturationValue = saturation.value;
    saturation.slider->setToolTip(tr("How strong the colours are. -100 % is black and white."));
    form()->addRow(tr("Saturation"), saturation.row);

    SliderRow temperature = makeSliderRow(this, kMinPercent, kMaxPercent, 1, 10);
    m_temperature = temperature.slider;
    m_temperatureValue = temperature.value;
    temperature.slider->setToolTip(tr("Warmer (+, more orange) or cooler (-, more blue)."));
    form()->addRow(tr("Temperature"), temperature.row);

    auto *buttonRow = new QWidget(this);
    auto *buttons = new QHBoxLayout(buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    m_reset = new QPushButton(tr("Reset"), buttonRow);
    m_reset->setToolTip(tr("Set all four back to 0: the clip as the camera recorded it."));
    auto *copy = new QPushButton(tr("Copy grade"), buttonRow);
    copy->setToolTip(tr("Copy this clip's four settings, to paste onto other clips."));
    m_paste = new QPushButton(tr("Paste grade"), buttonRow);
    m_paste->setToolTip(tr("Give this clip the copied settings. To paste onto every clip, "
                           "right-click a clip on the timeline."));
    m_paste->setEnabled(false);
    buttons->addWidget(m_reset);
    buttons->addWidget(copy);
    buttons->addWidget(m_paste);
    form()->addRow(buttonRow);
    connect(m_reset, &QPushButton::clicked, this, [this] { emit gradeEdited(ColorGrade{}); });
    connect(copy, &QPushButton::clicked, this, &ColourPanel::copyRequested);
    connect(m_paste, &QPushButton::clicked, this, &ColourPanel::pasteRequested);

    setHint(tr("Applies to this clip in playback and export."));

    setState(ColorGrade{});

    connect(m_exposure, &QSlider::valueChanged, this, [this](int v) {
        m_exposureValue->setText(tr("%1 EV").arg(v / kExposureStepsPerEv, 0, 'f', 1));
        emitIfEdited();
    });
    connect(m_contrast, &QSlider::valueChanged, this, [this](int v) {
        m_contrastValue->setText(tr("%1%").arg(v));
        emitIfEdited();
    });
    connect(m_saturation, &QSlider::valueChanged, this, [this](int v) {
        m_saturationValue->setText(tr("%1%").arg(v));
        emitIfEdited();
    });
    connect(m_temperature, &QSlider::valueChanged, this, [this](int v) {
        m_temperatureValue->setText(tr("%1%").arg(v));
        emitIfEdited();
    });
}

ColorGrade ColourPanel::currentGrade() const
{
    ColorGrade g;
    g.exposureEv = static_cast<float>(m_exposure->value()) / kExposureStepsPerEv;
    g.contrast = static_cast<float>(m_contrast->value());
    g.saturation = static_cast<float>(m_saturation->value());
    g.temperature = static_cast<float>(m_temperature->value());
    return g;
}

void ColourPanel::setState(const ColorGrade &grade)
{
    m_updating = true;
    setEnabled(true);

    const int ev = std::clamp(int(std::lround(grade.exposureEv * kExposureStepsPerEv)),
                              kMinExposureSteps, kMaxExposureSteps);
    m_exposure->setValue(ev);
    m_exposureValue->setText(tr("%1 EV").arg(ev / kExposureStepsPerEv, 0, 'f', 1));

    const int contrast = std::clamp(int(std::lround(grade.contrast)), kMinPercent, kMaxPercent);
    m_contrast->setValue(contrast);
    m_contrastValue->setText(tr("%1%").arg(contrast));

    const int saturation = std::clamp(int(std::lround(grade.saturation)), kMinPercent, kMaxPercent);
    m_saturation->setValue(saturation);
    m_saturationValue->setText(tr("%1%").arg(saturation));

    const int temperature = std::clamp(int(std::lround(grade.temperature)), kMinPercent, kMaxPercent);
    m_temperature->setValue(temperature);
    m_temperatureValue->setText(tr("%1%").arg(temperature));

    m_reset->setEnabled(currentGrade() != ColorGrade{});
    m_updating = false;
}

void ColourPanel::setNoClip()
{
    setEnabled(false);
}

void ColourPanel::setCanPaste(bool canPaste)
{
    m_paste->setEnabled(canPaste);
}

void ColourPanel::emitIfEdited()
{
    if (m_updating)
        return;
    m_reset->setEnabled(currentGrade() != ColorGrade{});
    emit gradeEdited(currentGrade());
}
