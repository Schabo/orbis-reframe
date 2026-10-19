#include "ReframePanel.h"

#include "EaseCombo.h"

#include <QComboBox>
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

    // The view's shape, keyframed like the pose itself. Everything but Flat
    // takes in most or all of the sphere, so the field of view acts as its
    // zoom rather than a crop.
    m_proj = new QComboBox(this);
    m_proj->addItem(tr("Flat"), int(Projection::Rectilinear));
    m_proj->addItem(tr("Natural view"), int(Projection::Panini));
    m_proj->addItem(tr("Fisheye"), int(Projection::Fisheye));
    m_proj->addItem(tr("Mirror ball"), int(Projection::MirrorBall));
    m_proj->addItem(tr("Globe"), int(Projection::Orthographic));
    m_proj->addItem(tr("Tiny planet"), int(Projection::TinyPlanet));
    m_proj->addItem(tr("Crystal ball"), int(Projection::CrystalBall));
    m_proj->setToolTip(tr("The shape of the picture. Flat is the usual view, natural view "
                          "keeps verticals straight when wide; fisheye draws the sphere as "
                          "a disc, the balls and planets show all of it."));
    form()->addRow(tr("View"), m_proj);
    connect(m_proj, &QComboBox::activated, this, &ReframePanel::emitIfEdited);


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
    p.projection = static_cast<Projection>(m_proj->currentData().toInt());
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
    m_proj->setCurrentIndex(m_proj->findData(int(pose.projection)));
    m_updating = false;
}

void ReframePanel::setClipUnkeyed(bool unkeyed)
{
    m_unkeyed->setVisible(unkeyed && !m_flat);
}

void ReframePanel::setFlatClip(bool flat, bool fill)
{
    m_flat = flat;
    for (QWidget *w :
         std::initializer_list<QWidget *>{m_yaw, m_pitch, m_roll, m_fov, m_proj,
                                        m_ease, m_track})
        form()->setRowVisible(w, !flat);
    form()->setRowVisible(m_trackUnavailable, !flat && !m_trackUnavailableReason.isEmpty());
    form()->setRowVisible(m_trackStatus, !flat && m_trackRunning);
    if (flat)
        m_unkeyed->hide();
    form()->setRowVisible(m_fit, flat);
    m_fit->setCurrentIndex(fill ? 1 : 0);
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
    for (QDoubleSpinBox *b : {m_yaw, m_pitch, m_roll, m_fov})
        b->setEnabled(!running);
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

void ReframePanel::emitIfEdited()
{
    if (!m_updating)
        emit poseEdited(currentPose());
}
