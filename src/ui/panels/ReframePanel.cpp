#include "ReframePanel.h"

#include "EaseCombo.h"

#include "../Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kRadToDeg = 180.0 / kPi;
constexpr double kDegToRad = kPi / 180.0;

QDoubleSpinBox *makeAngleBox(QWidget *parent, double lo, double hi, bool wrapping)
{
    auto *b = new QDoubleSpinBox(parent);
    b->setRange(lo, hi);
    b->setDecimals(1);
    b->setSingleStep(1.0);
    b->setSuffix(QStringLiteral("°"));
    b->setWrapping(wrapping);
    b->setKeyboardTracking(false); // emit once on commit, not per keystroke
    return b;
}

// A View button's shape: everything but the yaw, so the view keeps looking
// the same way. The values are the manual's recipes.
struct ViewPreset {
    const char *objectName;
    const char *icon;
    const char *name;
    const char *tip;
    double pitchDeg;
    double fovDeg; // corner to corner; 0: the default view's
    float distortion;
    float natural;
    float fisheye = 0.0f;
};
const ViewPreset kViewPresets[] = {
    {"reframePresetNormal", "view-normal",
     QT_TRANSLATE_NOOP("ReframePanel", "Normal"),
     QT_TRANSLATE_NOOP("ReframePanel", "the default view, level, 115° with Auto "
                                       "distortion."),
     0.0, 0.0, view::kAutoDistortion, 0.0f},
    {"reframePresetNatural", "view-natural",
     QT_TRANSLATE_NOOP("ReframePanel", "Natural"),
     QT_TRANSLATE_NOOP("ReframePanel", "a wide view with upright lines upright, "
                                       "level, 170° at 100 % distortion and full Natural."),
     0.0, 170.0, 1.0f, 1.0f},
    {"reframePresetFisheye", "view-fisheye",
     QT_TRANSLATE_NOOP("ReframePanel", "Fisheye"),
     QT_TRANSLATE_NOOP("ReframePanel", "a true fisheye, level, 180° at 100 % distortion "
                                       "and 100 % Fisheye."),
     0.0, 180.0, 1.0f, 0.0f, 1.0f},
    {"reframePresetMirrorBall", "view-mirror-ball",
     QT_TRANSLATE_NOOP("ReframePanel", "Mirror ball"),
     QT_TRANSLATE_NOOP("ReframePanel", "the whole world in a disc on black, level, 270° "
                                       "at 100 % distortion and 200 % Fisheye."),
     0.0, 270.0, 1.0f, 0.0f, 2.0f},
    {"reframePresetTinyPlanet", "view-tiny-planet",
     QT_TRANSLATE_NOOP("ReframePanel", "Tiny planet"),
     QT_TRANSLATE_NOOP("ReframePanel", "looking straight down, 300° at 100 % "
                                       "distortion."),
     -89.0, 300.0, 1.0f, 0.0f},
    {"reframePresetGlobe", "view-globe",
     QT_TRANSLATE_NOOP("ReframePanel", "Globe"),
     QT_TRANSLATE_NOOP("ReframePanel", "the world as a ball on black, looking straight "
                                       "down, 200° at 170 % distortion."),
     -89.0, 200.0, 1.7f, 0.0f},
    {"reframePresetTunnel", "view-tunnel",
     QT_TRANSLATE_NOOP("ReframePanel", "Tunnel"),
     QT_TRANSLATE_NOOP("ReframePanel", "the sky in the middle with the world round it, "
                                       "looking straight up, 300° at 100 % distortion."),
     89.0, 300.0, 1.0f, 0.0f},
    {"reframePresetCrystalBall", "view-crystal-ball",
     QT_TRANSLATE_NOOP("ReframePanel", "Crystal ball"),
     QT_TRANSLATE_NOOP("ReframePanel", "the sky in a ball on black, looking straight up, "
                                       "200° at 170 % distortion."),
     89.0, 200.0, 1.7f, 0.0f},
};
} // namespace

ReframePanel::ReframePanel(QWidget *parent)
    : SettingsPanel(tr("Reframe"), parent)
{
    // A split clip's pane, above the view fields: they steer the pane picked
    // here.
    m_pane = new QComboBox(this);
    m_pane->setObjectName(QStringLiteral("activePaneCombo"));
    m_pane->setToolTip(tr("The pane the view fields, a drag in the view and new keyframes "
                          "steer. Clicking a pane in the view picks it too."));
    form()->addRow(tr("Pane"), m_pane);
    connect(m_pane, &QComboBox::activated, this, [this](int i) { emit activePaneEdited(i); });

    // View: the whole view shape at once, above the fields it sets.
    m_presetRow = new QWidget(this);
    auto *presetLayout = new QHBoxLayout(m_presetRow);
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetLayout->setSpacing(2);
    for (const ViewPreset &preset : kViewPresets) {
        auto *b = new QToolButton(m_presetRow);
        b->setObjectName(QLatin1String(preset.objectName));
        b->setIcon(theme::icon(QLatin1String(preset.icon)));
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tr("%1: %2 The yaw stays.").arg(tr(preset.name), tr(preset.tip)));
        b->setAccessibleName(tr(preset.name));
        b->setAutoRaise(true);
        presetLayout->addWidget(b);
        connect(b, &QToolButton::clicked, this, [this, &preset] {
            edited([&preset](ReframePose &p) {
                p.pitch = static_cast<float>(preset.pitchDeg * kDegToRad);
                p.roll = 0.0f;
                p.fov = preset.fovDeg > 0.0 ? preset.fovDeg * kDegToRad : ReframePose{}.fov;
                p.distortion = preset.distortion;
                p.panini = preset.natural;
                p.fisheye = preset.fisheye;
            });
        });
    }
    presetLayout->addStretch(1);
    form()->addRow(tr("View"), m_presetRow);

    m_yaw = makeAngleBox(this, -180.0, 180.0, true);
    m_pitch = makeAngleBox(this, -89.0, 89.0, false);
    m_roll = makeAngleBox(this, -180.0, 180.0, true);
    m_fov = makeAngleBox(this, view::kMinFov * kRadToDeg, view::maxFov(1.0) * kRadToDeg, false);
    m_fov->setObjectName(QStringLiteral("reframeFov"));
    m_fov->setToolTip(tr("How much the view takes in, corner to corner. Wider than about "
                         "90° the picture bends to keep the edges from stretching (see "
                         "Distortion); looking down at 250° or more makes a tiny planet."));

    m_distortionRow = new QWidget(this);
    auto *distortionLayout = new QHBoxLayout(m_distortionRow);
    distortionLayout->setContentsMargins(0, 0, 0, 0);
    m_distortionAuto = new QCheckBox(tr("Auto"), m_distortionRow);
    m_distortionAuto->setObjectName(QStringLiteral("reframeDistortionAuto"));
    m_distortionAuto->setToolTip(tr("Follow the field of view: none up to about 95°, rising "
                                    "to 100 % for a tiny planet at 300° and wider."));
    m_distortion = new QSlider(Qt::Horizontal, m_distortionRow);
    m_distortion->setObjectName(QStringLiteral("reframeDistortion"));
    // 0-100 % from straight lines to kept shapes, then on to 200 % the globe:
    // the tiny planet's 100 % is the middle.
    m_distortion->setRange(0, static_cast<int>(view::kMaxDistortion * 100.0f));
    m_distortion->setPageStep(10);
    m_distortion->setTracking(false); // one edit (and keyframe) per release
    m_distortion->setToolTip(tr("0 % keeps straight lines straight but stretches the edges "
                                "of a wide view; 100 % keeps shapes and bends lines away from "
                                "the centre, the fisheye and tiny-planet look. Past 100 % the "
                                "world becomes a globe on black, seen from further away up "
                                "to 200 %."));
    m_distortionValue = new QLabel(m_distortionRow);
    m_distortionValue->setMinimumWidth(
        m_distortionValue->fontMetrics().horizontalAdvance(QStringLiteral("200%")));
    distortionLayout->addWidget(m_distortionAuto);
    distortionLayout->addWidget(m_distortion, 1);
    distortionLayout->addWidget(m_distortionValue);

    form()->addRow(tr("Yaw"), m_yaw);
    form()->addRow(tr("Pitch"), m_pitch);
    form()->addRow(tr("Roll"), m_roll);
    form()->addRow(tr("Field of view"), m_fov);
    form()->addRow(tr("Distortion"), m_distortionRow);

    // Natural, the Panini amount: 0-100 %, under the distortion it works with.
    // Full is the usual choice (on: 100 %, off: none); the slider fine-tunes,
    // and Full shows whether it is at 100 %.
    m_naturalRow = new QWidget(this);
    auto *naturalLayout = new QHBoxLayout(m_naturalRow);
    naturalLayout->setContentsMargins(0, 0, 0, 0);
    m_naturalFull = new QCheckBox(tr("Full"), m_naturalRow);
    m_naturalFull->setObjectName(QStringLiteral("reframeNaturalFull"));
    m_naturalFull->setToolTip(tr("Upright lines upright: Natural at 100 %, or none when "
                                 "unticked. The slider sets an amount in between."));
    m_natural = new QSlider(Qt::Horizontal, m_naturalRow);
    m_natural->setObjectName(QStringLiteral("reframeNatural"));
    m_natural->setRange(0, 100);
    m_natural->setPageStep(10);
    m_natural->setTracking(false); // one edit (and keyframe) per release
    m_natural->setToolTip(tr("Bends the picture only sideways: upright lines stay upright and "
                             "lines through the centre stay straight, while the width keeps "
                             "the Distortion's look. 100 % at 100 % distortion is the Panini "
                             "view, good for wide shots of streets and rooms. Does nothing at "
                             "0 % distortion."));
    m_naturalValue = new QLabel(m_naturalRow);
    m_naturalValue->setMinimumWidth(
        m_naturalValue->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
    naturalLayout->addWidget(m_naturalFull);
    naturalLayout->addWidget(m_natural, 1);
    naturalLayout->addWidget(m_naturalValue);
    form()->addRow(tr("Natural"), m_naturalRow);

    // Fisheye: 0-100 % from the view above to the equidistant fisheye, then
    // on to 200 % the mirror ball, like the distortion's globe.
    m_fisheyeRow = new QWidget(this);
    auto *fisheyeLayout = new QHBoxLayout(m_fisheyeRow);
    fisheyeLayout->setContentsMargins(0, 0, 0, 0);
    m_fisheye = new QSlider(Qt::Horizontal, m_fisheyeRow);
    m_fisheye->setObjectName(QStringLiteral("reframeFisheye"));
    m_fisheye->setRange(0, static_cast<int>(view::kMaxFisheye * 100.0f));
    m_fisheye->setPageStep(10);
    m_fisheye->setTracking(false); // one edit (and keyframe) per release
    m_fisheye->setToolTip(tr("Turns the view into a fisheye lens's picture: at 100 % a true "
                             "fisheye, which can take in the whole world, with what's behind "
                             "you round its rim. Past 100 % it becomes a mirror ball, the "
                             "whole world in a disc on black, at 200 %. From 100 % up, "
                             "Distortion and Natural only set the picture's size."));
    m_fisheyeValue = new QLabel(m_fisheyeRow);
    m_fisheyeValue->setMinimumWidth(
        m_fisheyeValue->fontMetrics().horizontalAdvance(QStringLiteral("200%")));
    fisheyeLayout->addWidget(m_fisheye, 1);
    fisheyeLayout->addWidget(m_fisheyeValue);
    form()->addRow(tr("Fisheye"), m_fisheyeRow);

    m_ease = easecombo::make(this);
    m_ease->setObjectName(QStringLiteral("reframeEaseCombo"));
    m_ease->setToolTip(tr("How the view moves from the key at the playhead (or the last one "
                          "before it) to the next key: at an even pace, starting fast and "
                          "slowing into the next key, or starting slowly and speeding up."));
    form()->addRow(tr("Easing"), m_ease);
    connect(m_ease, &QComboBox::activated, this,
            [this] { emit easeEdited(easecombo::ease(m_ease)); });

    m_track = new QPushButton(tr("Track subject…"), this);
    m_track->setObjectName(QStringLiteral("trackSubjectButton"));
    m_track->setCheckable(true);
    m_track->setToolTip(tr("Track subject  ( T ): draw a box around a subject in the view: "
                           "the camera follows it from the playhead on, written as reframe "
                           "keyframes you can edit. Esc cancels."));
    connect(m_track, &QPushButton::clicked, this, [this](bool checked) {
        if (m_trackRunning) {
            m_track->setChecked(true); // stays down until the run has wound up
            emit trackStopRequested();
        } else {
            emit trackSubjectToggled(checked);
        }
    });
    form()->addRow(m_track);
    m_trackUnavailable = new QLabel(this);
    m_trackUnavailable->setWordWrap(true);
    m_trackUnavailable->hide();
    form()->addRow(m_trackUnavailable);
    m_trackStatus = new QLabel(this);
    m_trackStatus->setObjectName(QStringLiteral("trackStatus"));
    m_trackStatus->setWordWrap(true);
    m_trackStatus->hide();
    form()->addRow(m_trackStatus);

    m_unkeyed = new QLabel(tr("⚠ This clip has no keyframes: it exports looking straight "
                              "forward, whatever the view shows now."),
                           this);
    m_unkeyed->setWordWrap(true);
    m_unkeyed->hide();
    form()->addRow(m_unkeyed);

    m_fit = new QComboBox(this);
    m_fit->setObjectName(QStringLiteral("reframeFitCombo"));
    m_fit->addItem(tr("Whole picture, with bars"));
    m_fit->addItem(tr("Fill the frame, edges cropped"));
    m_fit->setToolTip(tr("How this flat video fills the output frame when its shape "
                         "differs from it, e.g. a portrait phone video in a 16:9 export."));
    form()->addRow(tr("Fit"), m_fit);
    form()->setRowVisible(m_fit, false);
    connect(m_fit, &QComboBox::activated, this, [this](int i) { emit fitEdited(i == 1); });

    setClip(false);
    setPose(ReframePose{});
    setPaneLayout(pane::Layout::Single);

    connect(m_yaw, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        edited([v](ReframePose &p) { p.yaw = static_cast<float>(v * kDegToRad); });
    });
    connect(m_pitch, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        edited([v](ReframePose &p) { p.pitch = static_cast<float>(v * kDegToRad); });
    });
    connect(m_roll, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        edited([v](ReframePose &p) { p.roll = static_cast<float>(v * kDegToRad); });
    });
    connect(m_fov, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        edited([v](ReframePose &p) { p.fov = v * kDegToRad; });
    });
    // Unticking Auto keeps the distortion it showed, ticking it follows the
    // field of view again.
    connect(m_distortionAuto, &QCheckBox::toggled, this, [this](bool on) {
        edited([on](ReframePose &p) {
            p.distortion = on ? view::kAutoDistortion
                              : static_cast<float>(std::round(view::distortionOf(p) * 100.0) /
                                                   100.0);
        });
    });
    connect(m_distortion, &QSlider::sliderMoved, this,
            [this](int v) { m_distortionValue->setText(tr("%1%").arg(v)); });
    connect(m_distortion, &QSlider::valueChanged, this, [this](int v) {
        edited([v](ReframePose &p) {
            p.distortion = static_cast<float>(v) / 100.0f;
            // Less distortion reaches less wide.
            p.fov = std::min(p.fov, view::maxFov(p));
        });
    });
    connect(m_natural, &QSlider::sliderMoved, this,
            [this](int v) { m_naturalValue->setText(tr("%1%").arg(v)); });
    connect(m_natural, &QSlider::valueChanged, this, [this](int v) {
        edited([v](ReframePose &p) { p.panini = static_cast<float>(v) / 100.0f; });
    });
    connect(m_naturalFull, &QCheckBox::toggled, this, [this](bool on) {
        edited([on](ReframePose &p) { p.panini = on ? 1.0f : 0.0f; });
    });
    connect(m_fisheye, &QSlider::sliderMoved, this,
            [this](int v) { m_fisheyeValue->setText(tr("%1%").arg(v)); });
    connect(m_fisheye, &QSlider::valueChanged, this, [this](int v) {
        edited([v](ReframePose &p) { p.fisheye = static_cast<float>(v) / 100.0f; });
    });
}

void ReframePanel::edited(const std::function<void(ReframePose &)> &change)
{
    if (m_updating)
        return;
    ReframePose p = m_pose;
    change(p);
    showPose(p);
    emit poseEdited(p);
}

void ReframePanel::setPose(const ReframePose &pose)
{
    showPose(pose);
}

void ReframePanel::showPose(const ReframePose &pose)
{
    // Wrap yaw/roll into (-180, 180] so the spinboxes show a canonical value.
    const auto wrapDeg = [](double d) {
        d = std::fmod(d + 180.0, 360.0);
        if (d < 0.0)
            d += 360.0;
        return d - 180.0;
    };

    m_pose = pose;
    m_updating = true;
    m_yaw->setValue(wrapDeg(pose.yaw * kRadToDeg));
    m_pitch->setValue(pose.pitch * kRadToDeg);
    m_roll->setValue(wrapDeg(pose.roll * kRadToDeg));
    m_fov->setMaximum(view::maxFov(pose) * kRadToDeg);
    m_fov->setValue(pose.fov * kRadToDeg);
    const int percent = static_cast<int>(std::lround(view::distortionOf(pose) * 100.0));
    m_distortionAuto->setChecked(pose.distortionIsAuto());
    m_distortion->setEnabled(!pose.distortionIsAuto() && !m_trackRunning);
    m_distortion->setValue(percent);
    m_distortionValue->setText(tr("%1%").arg(percent));
    const int natural = static_cast<int>(std::lround(pose.panini * 100.0f));
    m_natural->setValue(natural);
    m_naturalValue->setText(tr("%1%").arg(natural));
    m_naturalFull->setChecked(natural >= 100);
    const int fisheye = static_cast<int>(std::lround(pose.fisheye * 100.0f));
    m_fisheye->setValue(fisheye);
    m_fisheyeValue->setText(tr("%1%").arg(fisheye));
    m_updating = false;
}

void ReframePanel::setPaneLayout(pane::Layout layout)
{
    m_paneLayout = layout;
    showPaneRow();
}

void ReframePanel::setActivePane(int pane)
{
    m_pane->setCurrentIndex(std::clamp(pane, 0, std::max(0, m_pane->count() - 1)));
}

void ReframePanel::showPaneRow()
{
    const pane::Layout l = m_paneLayout;
    // The panes by where they are, in pane order (PaneLayout.h).
    QStringList names;
    switch (l) {
    case pane::Layout::Single:
        break;
    case pane::Layout::SideBySide:
        names = {tr("Left"), tr("Right")};
        break;
    case pane::Layout::Stacked:
        names = {tr("Top"), tr("Bottom")};
        break;
    case pane::Layout::Grid:
        names = {tr("Top left"), tr("Top right"), tr("Bottom left"), tr("Bottom right")};
        break;
    }
    const int current = m_pane->currentIndex();
    if (names.size() != m_pane->count() ||
        (m_pane->count() > 0 && m_pane->itemText(0) != names.value(0))) {
        m_pane->clear();
        m_pane->addItems(names);
        m_pane->setCurrentIndex(std::clamp(current, 0, std::max(0, int(names.size()) - 1)));
    }
    const bool split = l != pane::Layout::Single;
    form()->setRowVisible(m_pane, !m_flat && split);
}

void ReframePanel::setClipUnkeyed(bool unkeyed)
{
    m_unkeyed->setText(m_paneLayout == pane::Layout::Single
                           ? tr("⚠ This clip has no keyframes: it exports looking straight "
                                "forward, whatever the view shows now.")
                           : tr("⚠ This pane has no keyframes: it exports looking straight "
                                "forward, whatever the view shows now."));
    m_unkeyed->setVisible(unkeyed && !m_flat);
}

void ReframePanel::setClip(bool flat, bool fill)
{
    m_hasClip = true;
    updateBodyEnabled();
    m_flat = flat;
    for (QWidget *w :
         std::initializer_list<QWidget *>{m_presetRow, m_yaw, m_pitch, m_roll, m_fov,
                                          m_distortionRow, m_naturalRow, m_fisheyeRow,
                                          m_ease, m_track})
        form()->setRowVisible(w, !flat);
    form()->setRowVisible(m_trackUnavailable, !flat && !m_trackUnavailableReason.isEmpty());
    form()->setRowVisible(m_trackStatus, !flat && m_trackRunning);
    if (flat)
        m_unkeyed->hide();
    form()->setRowVisible(m_fit, flat);
    m_fit->setCurrentIndex(fill ? 1 : 0);
    showPaneRow();
    setHint(flat ? tr("A flat video is shown as it is, not reframed.")
                 : tr("With Arm reframe authoring on in the edit toolbar, each value you enter "
                      "sets a keyframe at the playhead."));
}

void ReframePanel::setNoClip()
{
    setClip(false);
    setPaneLayout(pane::Layout::Single);
    m_hasClip = false;
    updateBodyEnabled();
}

void ReframePanel::updateBodyEnabled()
{
    setBodyEnabled(m_hasClip || m_trackRunning);
}

void ReframePanel::setEase(bool available, keytrack::Ease ease)
{
    m_easeAvailable = available;
    easecombo::show(m_ease, available && !m_trackRunning, ease);
}

void ReframePanel::setTrackUnavailable(const QString &reason)
{
    m_trackUnavailableReason = reason;
    m_trackUnavailable->setText(reason);
    // The row, not just the label: setClip may have hidden the row, and
    // a label shown in a hidden row isn't laid out (it lands top left).
    form()->setRowVisible(m_trackUnavailable, !reason.isEmpty() && !m_flat);
    if (!reason.isEmpty())
        m_track->setEnabled(false);
}

void ReframePanel::setTrackEnabled(bool enabled)
{
    m_track->setEnabled(enabled && m_trackUnavailableReason.isEmpty());
}

void ReframePanel::setTrackSelecting(bool selecting)
{
    m_track->setChecked(selecting);
}

void ReframePanel::setTrackRunning(bool running)
{
    m_trackRunning = running;
    updateBodyEnabled();
    m_track->setText(running ? tr("Stop tracking") : tr("Track subject…"));
    m_track->setChecked(running);
    m_track->setEnabled(true);
    form()->setRowVisible(m_trackStatus, running && !m_flat); // as setTrackUnavailable
    if (!running)
        m_trackStatus->clear();
    for (QWidget *w :
         std::initializer_list<QWidget *>{m_presetRow, m_yaw, m_pitch, m_roll, m_fov,
                                          m_distortionAuto, m_naturalFull, m_natural,
                                          m_fisheye, m_pane})
        w->setEnabled(!running);
    m_distortion->setEnabled(!running && !m_pose.distortionIsAuto());
    m_ease->setEnabled(m_easeAvailable && !running);
}

void ReframePanel::setTrackStatus(const QString &status)
{
    m_trackStatus->setText(status);
}

QWidget *ReframePanel::trackStopButton() const
{
    return m_track;
}

