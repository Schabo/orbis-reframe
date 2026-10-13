#include "ReframePanel.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>

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
    m_yaw = makeAngleBox(this, -180.0, 180.0, true);
    m_pitch = makeAngleBox(this, -89.0, 89.0, false);
    m_roll = makeAngleBox(this, -180.0, 180.0, true);
    m_fov = makeAngleBox(this, 20.0, 110.0, false);

    form()->addRow(tr("Yaw"), m_yaw);
    form()->addRow(tr("Pitch"), m_pitch);
    form()->addRow(tr("Roll"), m_roll);
    form()->addRow(tr("Field of view"), m_fov);

    m_track = new QPushButton(tr("Track subject…"), this);
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
    m_trackStatus->setWordWrap(true);
    m_trackStatus->hide();
    form()->addRow(m_trackStatus);

    m_unkeyed = new QLabel(tr("⚠ This clip has no keyframes: it exports looking straight "
                              "forward, whatever the view shows now."),
                           this);
    m_unkeyed->setWordWrap(true);
    m_unkeyed->hide();
    form()->addRow(m_unkeyed);

    setHint(tr("With Arm reframe authoring on in the edit toolbar, each value you enter sets "
               "a keyframe at the playhead."));

    setPose(ReframePose{});

    for (QDoubleSpinBox *b : {m_yaw, m_pitch, m_roll, m_fov})
        connect(b, &QDoubleSpinBox::valueChanged, this, &ReframePanel::emitIfEdited);
}

ReframePose ReframePanel::currentPose() const
{
    ReframePose p;
    p.yaw = static_cast<float>(m_yaw->value() * kDegToRad);
    p.pitch = static_cast<float>(m_pitch->value() * kDegToRad);
    p.roll = static_cast<float>(m_roll->value() * kDegToRad);
    p.fovY = static_cast<float>(m_fov->value() * kDegToRad);
    return p;
}

void ReframePanel::setPose(const ReframePose &pose)
{
    // Wrap yaw/roll into (-180, 180] so the spinboxes show a canonical value.
    const auto wrapDeg = [](double d) {
        d = std::fmod(d + 180.0, 360.0);
        if (d < 0.0)
            d += 360.0;
        return d - 180.0;
    };

    m_updating = true;
    m_yaw->setValue(wrapDeg(pose.yaw * kRadToDeg));
    m_pitch->setValue(pose.pitch * kRadToDeg);
    m_roll->setValue(wrapDeg(pose.roll * kRadToDeg));
    m_fov->setValue(pose.fovY * kRadToDeg);
    m_updating = false;
}

void ReframePanel::setClipUnkeyed(bool unkeyed)
{
    m_unkeyed->setVisible(unkeyed);
}

void ReframePanel::setTrackUnavailable(const QString &reason)
{
    m_trackUnavailableReason = reason;
    m_trackUnavailable->setText(reason);
    m_trackUnavailable->setVisible(!reason.isEmpty());
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
    m_trackStatus->setVisible(running);
    if (!running)
        m_trackStatus->clear();
    for (QDoubleSpinBox *b : {m_yaw, m_pitch, m_roll, m_fov})
        b->setEnabled(!running);
}

void ReframePanel::setTrackStatus(const QString &status)
{
    m_trackStatus->setText(status);
}

QWidget *ReframePanel::trackStopButton() const
{
    return m_track;
}

void ReframePanel::emitIfEdited()
{
    if (!m_updating)
        emit poseEdited(currentPose());
}
