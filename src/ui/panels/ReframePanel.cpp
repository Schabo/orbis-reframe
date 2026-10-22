#include "ReframePanel.h"

#include "EaseCombo.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

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
    m_distortionAuto->setToolTip(tr("Follow the field of view: none up to about 90°, rising "
                                    "to full at 170° and wider."));
    m_distortion = new QSlider(Qt::Horizontal, m_distortionRow);
    m_distortion->setObjectName(QStringLiteral("reframeDistortion"));
    m_distortion->setRange(0, 100);
    m_distortion->setPageStep(10);
    m_distortion->setTracking(false); // one edit (and keyframe) per release
    m_distortion->setToolTip(tr("0 % keeps straight lines straight but stretches the edges "
                                "of a wide view; 100 % keeps shapes and bends lines away from "
                                "the centre, the fisheye and tiny-planet look."));
    m_distortionValue = new QLabel(m_distortionRow);
    m_distortionValue->setMinimumWidth(
        m_distortionValue->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
    distortionLayout->addWidget(m_distortionAuto);
    distortionLayout->addWidget(m_distortion, 1);
    distortionLayout->addWidget(m_distortionValue);

    form()->addRow(tr("Yaw"), m_yaw);
    form()->addRow(tr("Pitch"), m_pitch);
    form()->addRow(tr("Roll"), m_roll);
    form()->addRow(tr("Field of view"), m_fov);
    form()->addRow(tr("Distortion"), m_distortionRow);

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

    setFlatClip(false);
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

void ReframePanel::setFlatClip(bool flat, bool fill)
{
    m_flat = flat;
    for (QWidget *w :
         std::initializer_list<QWidget *>{m_yaw, m_pitch, m_roll, m_fov, m_distortionRow,
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

void ReframePanel::setEase(bool available, keytrack::Ease ease)
{
    m_easeAvailable = available;
    easecombo::show(m_ease, available && !m_trackRunning, ease);
}

void ReframePanel::setTrackUnavailable(const QString &reason)
{
    m_trackUnavailableReason = reason;
    m_trackUnavailable->setText(reason);
    // The row, not just the label: setFlatClip may have hidden the row, and
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
    m_track->setText(running ? tr("Stop tracking") : tr("Track subject…"));
    m_track->setChecked(running);
    m_track->setEnabled(true);
    form()->setRowVisible(m_trackStatus, running && !m_flat); // as setTrackUnavailable
    if (!running)
        m_trackStatus->clear();
    for (QWidget *w :
         std::initializer_list<QWidget *>{m_yaw, m_pitch, m_roll, m_fov, m_distortionAuto, m_pane})
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

