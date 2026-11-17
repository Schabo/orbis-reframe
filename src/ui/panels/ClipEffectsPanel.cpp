#include "ClipEffectsPanel.h"

#include "PanelFields.h"
#include "../LookNames.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace {

// The look list's data: "preset:<id>" for a built-in look, else the .cube's
// path (absolute, so never starting with "preset:").
const QString kPresetPrefix = QStringLiteral("preset:");
// Its Load LUT... item.
constexpr int kLoadRole = Qt::UserRole + 1;

QString lookData(const fx::Look &l)
{
    return l.lut.empty() ? kPresetPrefix + QString::fromStdString(l.preset)
                         : QString::fromStdString(l.lut);
}

} // namespace

ClipEffectsPanel::ClipEffectsPanel(QWidget *parent) : SettingsPanel(tr("Effects"), parent)
{
    m_none = new QLabel(tr("No effects. Drag one from the Effects tab on the left onto the "
                           "clip, or double-click it there."),
                        this);
    m_none->setObjectName(QStringLiteral("effectsNone"));
    m_none->setWordWrap(true);
    form()->addRow(m_none);

    // In the order they run.
    m_look = addGroup(fx::kLook, tr("Colour look"), tr("Take the colour look off this clip."),
                      QStringLiteral("look"));
    m_lookChoice = new QComboBox(this);
    m_lookChoice->setObjectName(QStringLiteral("lookChoice"));
    m_lookChoice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_lookChoice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_lookChoice->setMinimumContentsLength(8);
    m_lookChoice->setToolTip(tr("A built-in look, or a 3D LUT (.cube) of your own, applied after "
                                "the Colour section's grade."));
    m_lookRow = m_lookChoice;
    form()->addRow(tr("Look"), m_lookChoice);
    m_lookProblem = new QLabel(this);
    m_lookProblem->setObjectName(QStringLiteral("lookProblem"));
    m_lookProblem->setWordWrap(true);
    form()->addRow(m_lookProblem);
    form()->setRowVisible(m_lookProblem, false);
    addSetting(m_look, tr("Strength"), 0, 100, QStringLiteral("lookStrength"),
               tr("How much of the look: 100 % is the look itself, 0 % the clip without it."));
    connect(m_lookChoice, &QComboBox::activated, this, [this](int index) {
        if (m_lookChoice->itemData(index, kLoadRole).toBool()) {
            if (m_shown.look)
                selectLook(*m_shown.look); // until a file is picked
            emit loadLookRequested();
            return;
        }
        emitIfEdited();
    });
    setLookFiles({});

    m_vignette = addGroup(fx::kVignette, tr("Vignette"), tr("Take the vignette off this clip."),
                          QStringLiteral("vignette"));
    addSetting(m_vignette, tr("Amount"), int(fx::Vignette::kMinAmount),
               int(fx::Vignette::kMaxAmount), QStringLiteral("vignetteAmount"),
               tr("Below 0 the edges get darker, -100 % black; above 0 lighter, 100 % white."));
    addSetting(m_vignette, tr("Size"), 0, 100, QStringLiteral("vignetteSize"),
               tr("Where it starts: 0 % in the middle of the picture, 100 % in its corners."));
    addSetting(m_vignette, tr("Softness"), 0, 100, QStringLiteral("vignetteSoftness"),
               tr("How gradually it comes in: 0 % a hard edge."));

    m_grain = addGroup(fx::kGrain, tr("Grain"), tr("Take the grain off this clip."),
                       QStringLiteral("grain"));
    addSetting(m_grain, tr("Amount"), 0, 100, QStringLiteral("grainAmount"),
               tr("How strong the grain is. It is strongest in the midtones; black and white "
                  "stay clean."));
    addSetting(m_grain, tr("Size"), int(fx::Grain::kMinSize), int(fx::Grain::kMaxSize),
               QStringLiteral("grainSize"),
               tr("How big a grain is: 100 % is one pixel of a 1080p video, and it scales "
                  "with the export size."));
    addSetting(m_grain, tr("Colour"), 0, 100, QStringLiteral("grainColour"),
               tr("0 % grey grain, the same in red, green and blue; 100 % each colour its "
                  "own."));

    setHint(tr("Applies to this clip in playback and export, after Colour."));
    setState({});
}

ClipEffectsPanel::Group ClipEffectsPanel::addGroup(const char *name, const QString &label,
                                                   const QString &removeTip, const QString &id)
{
    Group g;
    g.name = name;
    g.head = new QWidget(this);
    auto *head = new QHBoxLayout(g.head);
    head->setContentsMargins(0, 0, 0, 0);
    g.on = new QCheckBox(label, g.head);
    g.on->setObjectName(id + QStringLiteral("On"));
    g.on->setToolTip(tr("Untick to see the clip without it; its settings are kept."));
    g.remove = new QToolButton(g.head);
    g.remove->setObjectName(id + QStringLiteral("Remove"));
    g.remove->setText(tr("Remove"));
    g.remove->setToolTip(removeTip);
    head->addWidget(g.on, 1);
    head->addWidget(g.remove);
    form()->addRow(g.head);
    connect(g.on, &QCheckBox::toggled, this, [this] { emitIfEdited(); });
    connect(g.remove, &QToolButton::clicked, this, [this, name] {
        fx::ClipEffects e = currentEffects();
        e.remove(name);
        emit effectsEdited(e);
    });
    return g;
}

void ClipEffectsPanel::addSetting(Group &g, const QString &label, int lo, int hi,
                                  const QString &id, const QString &tip)
{
    Setting s;
    s.row = panelfields::sliderRow(this, lo, hi, &s.slider, &s.value);
    s.slider->setObjectName(id);
    s.slider->setToolTip(tip);
    s.value->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("-100%")));
    form()->addRow(label, s.row);
    connect(s.slider, &QSlider::valueChanged, this, [this, value = s.value](int v) {
        value->setText(tr("%1%").arg(v));
        emitIfEdited();
    });
    g.settings.push_back(s);
}

float ClipEffectsPanel::value(const Group &g, size_t i) const
{
    return float(g.settings[i].slider->value());
}

fx::ClipEffects ClipEffectsPanel::currentEffects() const
{
    fx::ClipEffects e = m_shown;
    if (e.look) {
        e.look->on = m_look.on->isChecked();
        e.look->strength = value(m_look, 0);
        const QString data = m_lookChoice->currentData().toString();
        if (data.startsWith(kPresetPrefix)) {
            e.look->preset = data.mid(kPresetPrefix.size()).toStdString();
            e.look->lut.clear();
        } else if (!data.isEmpty()) {
            e.look->lut = data.toStdString();
            e.look->preset.clear();
        }
    }
    if (e.vignette) {
        e.vignette->on = m_vignette.on->isChecked();
        e.vignette->amount = value(m_vignette, 0);
        e.vignette->size = value(m_vignette, 1);
        e.vignette->softness = value(m_vignette, 2);
    }
    if (e.grain) {
        e.grain->on = m_grain.on->isChecked();
        e.grain->amount = value(m_grain, 0);
        e.grain->size = value(m_grain, 1);
        e.grain->colour = value(m_grain, 2);
    }
    return e;
}

void ClipEffectsPanel::showGroup(const Group &g, bool present, bool on,
                                 std::initializer_list<float> values)
{
    form()->setRowVisible(g.head, present);
    g.on->setChecked(on);
    size_t i = 0;
    for (float x : values) {
        const Setting &s = g.settings[i++];
        form()->setRowVisible(s.row, present);
        const int v = std::clamp(int(std::lround(x)), s.slider->minimum(), s.slider->maximum());
        s.slider->setValue(v);
        s.value->setText(tr("%1%").arg(v));
    }
}

void ClipEffectsPanel::setState(const fx::ClipEffects &effects)
{
    m_updating = true;
    setBodyEnabled(true);
    m_shown = effects;
    form()->setRowVisible(m_none, effects.isEmpty());
    const fx::Look l = effects.look.value_or(fx::Look{});
    showGroup(m_look, effects.look.has_value(), l.on, {l.strength});
    form()->setRowVisible(m_lookRow, effects.look.has_value());
    if (!effects.look)
        setLookProblem({});
    selectLook(l);
    const fx::Vignette v = effects.vignette.value_or(fx::Vignette{});
    showGroup(m_vignette, effects.vignette.has_value(), v.on, {v.amount, v.size, v.softness});
    const fx::Grain g = effects.grain.value_or(fx::Grain{});
    showGroup(m_grain, effects.grain.has_value(), g.on, {g.amount, g.size, g.colour});
    enableSettings(effects);
    m_updating = false;
}

void ClipEffectsPanel::enableSettings(const fx::ClipEffects &e)
{
    for (const auto &[g, on] : {std::pair{&m_look, !e.look || e.look->on},
                                std::pair{&m_vignette, !e.vignette || e.vignette->on},
                                std::pair{&m_grain, !e.grain || e.grain->on}})
        for (const Setting &s : g->settings)
            s.slider->setEnabled(on);
    m_lookChoice->setEnabled(!e.look || e.look->on);
}

void ClipEffectsPanel::setLookFiles(const QStringList &paths)
{
    m_lookFiles = paths;
    const bool was = m_updating;
    m_updating = true;
    m_lookChoice->clear();
    for (const char *id : color::look::kPresets)
        m_lookChoice->addItem(looknames::name(QString::fromLatin1(id)),
                              kPresetPrefix + QString::fromLatin1(id));
    for (const QString &p : m_lookFiles) {
        m_lookChoice->addItem(QFileInfo(p).completeBaseName(), p);
        m_lookChoice->setItemData(m_lookChoice->count() - 1, p, Qt::ToolTipRole);
    }
    m_lookChoice->insertSeparator(m_lookChoice->count());
    m_lookChoice->addItem(tr("Load LUT…"));
    m_lookChoice->setItemData(m_lookChoice->count() - 1, true, kLoadRole);
    selectLook(m_shown.look.value_or(fx::Look{}));
    m_updating = was;
}

void ClipEffectsPanel::selectLook(const fx::Look &look)
{
    const QString data = lookData(look);
    int i = m_lookChoice->findData(data);
    if (i < 0) { // not listed: a file from elsewhere, or a later version's look
        i = m_lookChoice->count() - 2; // before the separator
        const QString name =
            look.lut.empty()
                ? tr("%1 (not in this version)").arg(QString::fromStdString(look.preset))
                : QFileInfo(data).completeBaseName();
        m_lookChoice->insertItem(i, name, data);
        m_lookChoice->setItemData(i, data, Qt::ToolTipRole);
    }
    const QSignalBlocker block(m_lookChoice);
    m_lookChoice->setCurrentIndex(i);
}

void ClipEffectsPanel::setLookProblem(const QString &problem)
{
    m_lookProblem->setText(problem);
    form()->setRowVisible(m_lookProblem, !problem.isEmpty());
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
    enableSettings(e);
    if (e != m_shown) {
        m_shown = e;
        emit effectsEdited(e);
    }
}
