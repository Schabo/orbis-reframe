#include "ClipEffectsPanel.h"

#include "PanelFields.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>

#include <algorithm>
#include <cmath>

ClipEffectsPanel::ClipEffectsPanel(QWidget *parent) : SettingsPanel(tr("Effects"), parent)
{
    m_none = new QLabel(tr("No effects. Drag one from the Effects tab on the left onto the "
                           "clip, or double-click it there."),
                        this);
    m_none->setObjectName(QStringLiteral("effectsNone"));
    m_none->setWordWrap(true);
    form()->addRow(m_none);

    // Vignette: its heading -- the name, on / off, Remove -- then its rows.
    m_vignetteHead = new QWidget(this);
    auto *head = new QHBoxLayout(m_vignetteHead);
    head->setContentsMargins(0, 0, 0, 0);
    m_vignetteOn = new QCheckBox(tr("Vignette"), m_vignetteHead);
    m_vignetteOn->setObjectName(QStringLiteral("vignetteOn"));
    m_vignetteOn->setToolTip(tr("Untick to see the clip without it; its settings are kept."));
    m_vignetteRemove = new QToolButton(m_vignetteHead);
    m_vignetteRemove->setObjectName(QStringLiteral("vignetteRemove"));
    m_vignetteRemove->setText(tr("Remove"));
    m_vignetteRemove->setToolTip(tr("Take the vignette off this clip."));
    head->addWidget(m_vignetteOn, 1);
    head->addWidget(m_vignetteRemove);
    form()->addRow(m_vignetteHead);

    m_vignetteAmountRow = panelfields::sliderRow(this, int(fx::Vignette::kMinAmount),
                                                 int(fx::Vignette::kMaxAmount), &m_vignetteAmount,
                                                 &m_vignetteAmountValue);
    m_vignetteAmount->setObjectName(QStringLiteral("vignetteAmount"));
    m_vignetteAmountValue->setMinimumWidth(
        fontMetrics().horizontalAdvance(QStringLiteral("-100%")));
    m_vignetteAmount->setToolTip(tr("Below 0 the edges get darker, -100 % black; above 0 "
                                    "lighter, 100 % white."));
    form()->addRow(tr("Amount"), m_vignetteAmountRow);
    m_vignetteSizeRow =
        panelfields::sliderRow(this, 0, 100, &m_vignetteSize, &m_vignetteSizeValue);
    m_vignetteSize->setObjectName(QStringLiteral("vignetteSize"));
    m_vignetteSize->setToolTip(tr("Where it starts: 0 % in the middle of the picture, 100 % "
                                  "in its corners."));
    form()->addRow(tr("Size"), m_vignetteSizeRow);
    m_vignetteSoftnessRow =
        panelfields::sliderRow(this, 0, 100, &m_vignetteSoftness, &m_vignetteSoftnessValue);
    m_vignetteSoftness->setObjectName(QStringLiteral("vignetteSoftness"));
    m_vignetteSoftness->setToolTip(tr("How gradually it comes in: 0 % a hard edge."));
    form()->addRow(tr("Softness"), m_vignetteSoftnessRow);

    setHint(tr("Applies to this clip in playback and export, after Colour."));
    setState({});

    const auto percent = [this](QSlider *s, QLabel *value) {
        connect(s, &QSlider::valueChanged, this, [this, value](int v) {
            value->setText(tr("%1%").arg(v));
            emitIfEdited();
        });
    };
    percent(m_vignetteAmount, m_vignetteAmountValue);
    percent(m_vignetteSize, m_vignetteSizeValue);
    percent(m_vignetteSoftness, m_vignetteSoftnessValue);
    connect(m_vignetteOn, &QCheckBox::toggled, this, [this] { emitIfEdited(); });
    connect(m_vignetteRemove, &QToolButton::clicked, this, [this] {
        fx::ClipEffects e = currentEffects();
        e.remove(fx::kVignette);
        emit effectsEdited(e);
    });
}

fx::ClipEffects ClipEffectsPanel::currentEffects() const
{
    fx::ClipEffects e = m_shown;
    if (e.vignette) {
        e.vignette->on = m_vignetteOn->isChecked();
        e.vignette->amount = float(m_vignetteAmount->value());
        e.vignette->size = float(m_vignetteSize->value());
        e.vignette->softness = float(m_vignetteSoftness->value());
    }
    return e;
}

void ClipEffectsPanel::setState(const fx::ClipEffects &effects)
{
    m_updating = true;
    setBodyEnabled(true);
    m_shown = effects;
    const bool vignette = effects.vignette.has_value();
    form()->setRowVisible(m_none, effects.isEmpty());
    for (QWidget *row : {m_vignetteHead, m_vignetteAmountRow, m_vignetteSizeRow,
                         m_vignetteSoftnessRow})
        form()->setRowVisible(row, vignette);
    const fx::Vignette v = effects.vignette.value_or(fx::Vignette{});
    m_vignetteOn->setChecked(v.on);
    const auto show = [this](QSlider *s, QLabel *value, float x) {
        const int i = std::clamp(int(std::lround(x)), s->minimum(), s->maximum());
        s->setValue(i);
        value->setText(tr("%1%").arg(i));
    };
    show(m_vignetteAmount, m_vignetteAmountValue, v.amount);
    show(m_vignetteSize, m_vignetteSizeValue, v.size);
    show(m_vignetteSoftness, m_vignetteSoftnessValue, v.softness);
    for (QWidget *w : {static_cast<QWidget *>(m_vignetteAmount),
                       static_cast<QWidget *>(m_vignetteSize),
                       static_cast<QWidget *>(m_vignetteSoftness)})
        w->setEnabled(v.on);
    m_updating = false;
}

void ClipEffectsPanel::setNoClip()
{
    setState({});
    setBodyEnabled(false);
}

void ClipEffectsPanel::emitIfEdited()
{
    if (m_updating)
        return;
    const fx::ClipEffects e = currentEffects();
    for (QWidget *w : {static_cast<QWidget *>(m_vignetteAmount),
                       static_cast<QWidget *>(m_vignetteSize),
                       static_cast<QWidget *>(m_vignetteSoftness)})
        w->setEnabled(!e.vignette || e.vignette->on);
    if (e != m_shown) {
        m_shown = e;
        emit effectsEdited(e);
    }
}
