#include "StabilizationPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

// "50% · 0.90 s": a strength slider's position and the window it maps to.
QString strengthLabel(int pct, float windowSec)
{
    return QStringLiteral("%1% · %2 s").arg(pct).arg(windowSec, 0, 'f', windowSec < 10.0f ? 2 : 1);
}

QDoubleSpinBox *secBox(double def)
{
    auto *b = new QDoubleSpinBox;
    b->setRange(0.05, 5.0);
    b->setSingleStep(0.1);
    b->setDecimals(2);
    b->setSuffix(QStringLiteral(" s"));
    b->setValue(def);
    return b;
}
QDoubleSpinBox *degBox()
{
    auto *b = new QDoubleSpinBox;
    b->setRange(-45.0, 45.0);
    b->setSingleStep(0.5);
    b->setDecimals(1);
    b->setSuffix(QStringLiteral(" °"));
    b->setValue(0.0);
    return b;
}
} // namespace

StabilizationPanel::StabilizationPanel(QWidget *parent)
    : SettingsPanel(tr("Stabilization"), parent)
{
    m_mode = new QComboBox(this);
    m_mode->setObjectName(QStringLiteral("stabilizationMode"));
    m_mode->addItem(tr("Off"), int(Mode::Off));
    m_mode->addItem(tr("Lean follow"), int(Mode::LeanFollow));
    m_mode->addItem(tr("Horizon lock"), int(Mode::HorizonLock));
    m_mode->addItem(tr("Direction lock"), int(Mode::DirectionLock));
    m_mode->addItem(tr("Custom"), int(Mode::Custom));
    form()->addRow(tr("Mode"), m_mode);

    // Lean follow: one Strength slider standing in for the shared smoothing
    // window (insv::leanWindowFromStrength). Left = follow the ride closely,
    // right = steadier; a slow cornering lean is kept at any setting.
    m_leanRow = new QWidget(this);
    auto *lr = new QHBoxLayout(m_leanRow);
    lr->setContentsMargins(0, 0, 0, 0);
    m_leanStrength = new QSlider(Qt::Horizontal, m_leanRow);
    m_leanStrength->setRange(0, 100);
    m_leanStrength->setSingleStep(5);
    m_leanStrength->setPageStep(10);
    m_leanStrength->setTickPosition(QSlider::TicksBelow);
    m_leanStrength->setTickInterval(25);
    m_leanStrength->setToolTip(tr("To the left, the view follows the ride more closely; to the "
                                  "right, it's steadier. The middle suits most riding and "
                                  "keeps a slow lean into a corner. The top of the range is "
                                  "for very shaky footage, and starts to flatten the lean as "
                                  "well."));
    m_leanStrengthValue = new QLabel(m_leanRow);
    m_leanStrengthValue->setMinimumWidth(
        fontMetrics().horizontalAdvance(QStringLiteral("100% · 10.0 s")));
    lr->addWidget(m_leanStrength, 1);
    lr->addWidget(m_leanStrengthValue);
    form()->addRow(tr("Strength"), m_leanRow);

    // Horizon lock: one Amount slider mapping to horizonLockAmount (0 = keep the
    // current tilt .. 100 % = fully level). Uses the accel horizon reference.
    m_horizonRow = new QWidget(this);
    auto *hr = new QHBoxLayout(m_horizonRow);
    hr->setContentsMargins(0, 0, 0, 0);
    m_horizonAmount = new QSlider(Qt::Horizontal, m_horizonRow);
    m_horizonAmount->setRange(0, 100);
    m_horizonAmount->setSingleStep(5);
    m_horizonAmount->setPageStep(10);
    m_horizonAmount->setTickPosition(QSlider::TicksBelow);
    m_horizonAmount->setTickInterval(25);
    m_horizonAmount->setToolTip(tr("How level to hold the horizon. At 100 % the view never "
                                   "tilts; lower values let some of the camera's own tilt back "
                                   "in."));
    m_horizonAmountValue = new QLabel(m_horizonRow);
    m_horizonAmountValue->setMinimumWidth(
        fontMetrics().horizontalAdvance(QStringLiteral("100% · 10.0 s")));
    hr->addWidget(m_horizonAmount, 1);
    hr->addWidget(m_horizonAmountValue);
    form()->addRow(tr("Amount"), m_horizonRow);

    // Horizon lock: Smoothing slider for the heading window
    // (insv::horizonWindowFromStrength). Left = the view follows turns
    // closely, right = holds its heading longer (calmer, lags in corners).
    m_horizonSmoothRow = new QWidget(this);
    auto *hsr = new QHBoxLayout(m_horizonSmoothRow);
    hsr->setContentsMargins(0, 0, 0, 0);
    m_horizonSmooth = new QSlider(Qt::Horizontal, m_horizonSmoothRow);
    m_horizonSmooth->setRange(0, 100);
    m_horizonSmooth->setSingleStep(5);
    m_horizonSmooth->setPageStep(10);
    m_horizonSmooth->setTickPosition(QSlider::TicksBelow);
    m_horizonSmooth->setTickInterval(25);
    m_horizonSmooth->setToolTip(tr("How quickly the view turns when you change direction. To "
                                   "the left it follows turns closely; to the right it's "
                                   "calmer, but lags behind in corners."));
    m_horizonSmoothValue = new QLabel(m_horizonSmoothRow);
    m_horizonSmoothValue->setMinimumWidth(
        fontMetrics().horizontalAdvance(QStringLiteral("100% · 10.0 s")));
    hsr->addWidget(m_horizonSmooth, 1);
    hsr->addWidget(m_horizonSmoothValue);
    form()->addRow(tr("Smoothing"), m_horizonSmoothRow);

    m_customRows = new QWidget(this);
    auto *cf = new QFormLayout(m_customRows);
    cf->setContentsMargins(0, 0, 0, 0);
    m_winYaw = secBox(0.9);
    m_winPitch = secBox(0.9);
    m_winRoll = secBox(0.9);
    m_lockAmount = new QDoubleSpinBox;
    m_lockAmount->setRange(0.0, 1.0);
    m_lockAmount->setSingleStep(0.05);
    m_lockAmount->setDecimals(2);
    m_pitchOffset = degBox();
    m_rollOffset = degBox();
    m_autoLevel = new QCheckBox(tr("Level to mount's average"));
    m_autoLevel->setToolTip(tr("Treat the camera's average position over the clip as level, so "
                               "a crooked mount still gives a level horizon. Off: the camera "
                               "body's own \"up\" is level."));
    const QString smoothTip = tr("Smoothing time for %1. Short follows the camera closely; long "
                                 "holds the view still.");
    m_winYaw->setToolTip(smoothTip.arg(tr("turning left and right")));
    m_winPitch->setToolTip(smoothTip.arg(tr("tilting up and down")));
    m_winRoll->setToolTip(smoothTip.arg(tr("tilting sideways")));
    m_lockAmount->setToolTip(tr("0 keeps the camera's lean, 1 holds the horizon fully level."));
    const QString offsetTip = tr("Tilts the result by a fixed angle, for example to correct a "
                                 "camera that was mounted crooked.");
    m_pitchOffset->setToolTip(offsetTip);
    m_rollOffset->setToolTip(offsetTip);
    cf->addRow(tr("Smooth yaw"), m_winYaw);
    cf->addRow(tr("Smooth pitch"), m_winPitch);
    cf->addRow(tr("Smooth roll"), m_winRoll);
    cf->addRow(tr("Level horizon"), m_lockAmount); // 0 = keep lean .. 1 = level
    cf->addRow(tr("Pitch offset"), m_pitchOffset);
    cf->addRow(tr("Roll offset"), m_rollOffset);
    cf->addRow(tr("Auto-level"), m_autoLevel);
    m_customLockDirection = new QCheckBox(tr("Hold one direction"));
    m_customLockDirection->setObjectName(QStringLiteral("stabilizationCustomLockDirection"));
    m_customLockDirection->setToolTip(
        tr("Keep the view pointing one way, however the camera turns, as Direction lock does: "
           "the way the camera faced at the playhead when you tick it."));
    cf->addRow(tr("Lock direction"), m_customLockDirection);
    form()->addRow(m_customRows);

    // Direction lock, or Custom with Lock direction: which way the view is
    // held, and a button to take the direction under the playhead instead.
    m_directionRow = new QWidget(this);
    auto *dr = new QVBoxLayout(m_directionRow);
    dr->setContentsMargins(0, 0, 0, 0);
    m_directionStatus = new QLabel(m_directionRow);
    m_directionStatus->setObjectName(QStringLiteral("stabilizationDirectionStatus"));
    m_directionStatus->setWordWrap(true);
    auto *lockDirection = new QPushButton(tr("Lock to this direction"), m_directionRow);
    lockDirection->setObjectName(QStringLiteral("stabilizationLockDirection"));
    lockDirection->setToolTip(tr("Keep the view pointing the way the camera faced at the "
                                 "playhead, however the camera turns."));
    auto *buttonLine = new QHBoxLayout;
    buttonLine->addWidget(lockDirection);
    buttonLine->addStretch(1);
    dr->addWidget(m_directionStatus);
    dr->addLayout(buttonLine);
    form()->addRow(m_directionRow);
    connect(lockDirection, &QPushButton::clicked, this,
            &StabilizationPanel::lockDirectionRequested);


    // Accel-fusion low-pass: cutoff (Hz) for the accelerometer before it feeds
    // the horizon-leveling Mahony term. The stick mount leaks ~11 Hz resonance
    // onto the accel; without this it rides into the "leveled" orientation as
    // jitter. Only affects modes that level toward the accel horizon (Horizon
    // lock / Custom); LeanFollow output is accel-independent.
    m_accelLpRow = new QWidget(this);
    auto *alpl = new QHBoxLayout(m_accelLpRow);
    alpl->setContentsMargins(0, 0, 0, 0);
    m_accelLowPass = new QDoubleSpinBox(m_accelLpRow);
    m_accelLowPass->setRange(0.0, 20.0);
    m_accelLowPass->setSingleStep(0.5);
    m_accelLowPass->setDecimals(1);
    m_accelLowPass->setSuffix(QStringLiteral(" Hz"));
    m_accelLowPass->setValue(insv::StabConfig::preset(insv::StabMode::HorizonLock).accelLowPassHz);
    m_accelLowPass->setToolTip(tr("Filters vibration out of the camera's motion sensor before "
                                  "it's used to find the horizon. If the horizon trembles on a "
                                  "vibrating mount, try a lower value; 0 turns the filter off."));
    alpl->addWidget(m_accelLowPass, 1);
    form()->addRow(tr("Vibration filter"), m_accelLpRow);

    // Sync fine-tune status, set by MainWindow for the active clip.
    m_syncStatus = new QLabel(this);
    m_syncStatus->setWordWrap(true);
    m_syncStatus->setToolTip(tr("To remove fast shake, the gyroscope has to line up with the "
                                "picture to within about a millisecond. orbis-reframe measures "
                                "this for each clip from the video and saves it with the "
                                "project."));
    m_syncStatus->hide();
    form()->addRow(m_syncStatus);

    auto *buttonRow = new QWidget(this);
    auto *buttons = new QHBoxLayout(buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    auto *copy = new QPushButton(tr("Copy stabilization"), buttonRow);
    copy->setToolTip(tr("Copy this clip's stabilization setting, to paste onto other clips."));
    m_paste = new QPushButton(tr("Paste stabilization"), buttonRow);
    m_paste->setToolTip(tr("Give this clip the copied setting. To paste onto every clip, "
                           "right-click a clip on the timeline."));
    m_paste->setEnabled(false);
    buttons->addWidget(copy);
    buttons->addWidget(m_paste);
    buttons->addStretch(1);
    form()->addRow(buttonRow);
    connect(copy, &QPushButton::clicked, this, &StabilizationPanel::copyRequested);
    connect(m_paste, &QPushButton::clicked, this, &StabilizationPanel::pasteRequested);

    setHint(tr("Steadies this clip with the camera's own motion sensors, in playback and "
               "export. Lean follow keeps a motorcycle's lean in corners; Horizon lock keeps "
               "the horizon level; Direction lock also keeps the view pointing one way."));

    setState(Mode::Off, insv::StabConfig::preset(insv::StabMode::LeanFollow));

    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] {
        updateModeVisibility();
        emitIfEdited();
    });
    connect(m_leanStrength, &QSlider::valueChanged, this, [this](int v) {
        m_leanStrengthValue->setText(strengthLabel(v, insv::leanWindowFromStrength(v / 100.0f)));
        emitIfEdited();
    });
    connect(m_horizonSmooth, &QSlider::valueChanged, this, [this](int v) {
        m_horizonSmoothValue->setText(
            strengthLabel(v, insv::horizonWindowFromStrength(v / 100.0f)));
        emitIfEdited();
    });
    connect(m_horizonAmount, &QSlider::valueChanged, this, [this](int v) {
        m_horizonAmountValue->setText(QStringLiteral("%1%").arg(v));
        emitIfEdited();
    });
    for (QDoubleSpinBox *b : {m_winYaw, m_winPitch, m_winRoll, m_lockAmount, m_pitchOffset,
                              m_rollOffset, m_accelLowPass})
        connect(b, &QDoubleSpinBox::valueChanged, this, &StabilizationPanel::emitIfEdited);
    connect(m_autoLevel, &QCheckBox::toggled, this, &StabilizationPanel::emitIfEdited);
    connect(m_customLockDirection, &QCheckBox::toggled, this, [this] {
        updateModeVisibility();
        emitIfEdited();
    });
}

void StabilizationPanel::updateModeVisibility()
{
    const int m = m_mode->currentData().toInt();
    // setRowVisible, not the widget's setVisible: that would leave the row's
    // label showing.
    form()->setRowVisible(m_leanRow, m == int(Mode::LeanFollow));
    const bool level = m == int(Mode::HorizonLock) || m == int(Mode::DirectionLock);
    form()->setRowVisible(m_horizonRow, level);
    form()->setRowVisible(m_horizonSmoothRow, m == int(Mode::HorizonLock));
    form()->setRowVisible(m_directionRow,
                          m == int(Mode::DirectionLock) ||
                              (m == int(Mode::Custom) && m_customLockDirection->isChecked()));
    form()->setRowVisible(m_customRows, m == int(Mode::Custom));
    form()->setRowVisible(m_accelLpRow, level || m == int(Mode::Custom));
}

insv::StabConfig StabilizationPanel::nativeConfigForMode() const
{
    const auto mode = Mode(m_mode->currentData().toInt());
    const float accelLp = float(m_accelLowPass->value()); // Horizon lock / Custom only
    if (mode == Mode::LeanFollow) {
        insv::StabConfig c = insv::StabConfig::preset(insv::StabMode::LeanFollow);
        const float w = insv::leanWindowFromStrength(float(m_leanStrength->value()) / 100.0f);
        c.windowYawSec = c.windowPitchSec = c.windowRollSec = w;
        return c;
    }
    if (mode == Mode::HorizonLock) {
        insv::StabConfig c = insv::StabConfig::preset(insv::StabMode::HorizonLock);
        c.horizonLockAmount = float(m_horizonAmount->value()) / 100.0f;
        c.windowYawSec = insv::horizonWindowFromStrength(float(m_horizonSmooth->value()) / 100.0f);
        c.accelLowPassHz = accelLp;
        return c;
    }
    if (mode == Mode::DirectionLock) {
        insv::StabConfig c = insv::StabConfig::preset(insv::StabMode::DirectionLock);
        c.horizonLockAmount = float(m_horizonAmount->value()) / 100.0f;
        c.accelLowPassHz = accelLp;
        c.directionSec = m_directionSec;
        return c;
    }
    if (mode == Mode::Custom) {
        insv::StabConfig c = insv::StabConfig::preset(insv::StabMode::Custom);
        c.windowYawSec = float(m_winYaw->value());
        c.windowPitchSec = float(m_winPitch->value());
        c.windowRollSec = float(m_winRoll->value());
        c.horizonLockAmount = float(m_lockAmount->value());
        c.pitchOffsetRad = float(m_pitchOffset->value() * kDegToRad);
        c.rollOffsetRad = float(m_rollOffset->value() * kDegToRad);
        c.autoLevel = m_autoLevel->isChecked();
        c.lockDirection = m_customLockDirection->isChecked();
        c.directionSec = m_directionSec;
        c.accelLowPassHz = accelLp;
        return c;
    }
    return insv::StabConfig{}; // Off -> no native stab
}

void StabilizationPanel::setState(Mode mode, const insv::StabConfig &cfg)
{
    setBodyEnabled(true);
    m_updating = true;
    const int idx = m_mode->findData(int(mode));
    m_mode->setCurrentIndex(idx < 0 ? 0 : idx);

    // Lean-follow Strength: invert the shared window when that's the mode we're
    // reflecting; otherwise sit at the default so switching to Lean follow lands
    // on the tuned preset.
    const float strength01 = mode == Mode::LeanFollow
                                 ? insv::leanStrengthFromWindow(cfg.windowRollSec)
                                 : 0.5f;
    const int pct = int(std::lround(strength01 * 100.0f));
    m_leanStrength->setValue(pct);
    m_leanStrengthValue->setText(strengthLabel(pct, insv::leanWindowFromStrength(pct / 100.0f)));

    const int hpct = mode == Mode::HorizonLock || mode == Mode::DirectionLock
                         ? std::clamp(int(std::lround(cfg.horizonLockAmount * 100.0f)), 0, 100)
                         : 100;
    m_horizonAmount->setValue(hpct);
    m_horizonAmountValue->setText(QStringLiteral("%1%").arg(hpct));

    // Horizon-lock Smoothing: same pattern as the Lean Strength above.
    const float hs01 = mode == Mode::HorizonLock ? insv::horizonStrengthFromWindow(cfg.windowYawSec)
                                                 : 0.5f;
    const int hspct = int(std::lround(hs01 * 100.0f));
    m_horizonSmooth->setValue(hspct);
    m_horizonSmoothValue->setText(
        strengthLabel(hspct, insv::horizonWindowFromStrength(hspct / 100.0f)));

    m_winYaw->setValue(cfg.windowYawSec);
    m_winPitch->setValue(cfg.windowPitchSec);
    m_winRoll->setValue(cfg.windowRollSec);
    m_lockAmount->setValue(cfg.horizonLockAmount);
    m_pitchOffset->setValue(cfg.pitchOffsetRad * kRadToDeg);
    m_rollOffset->setValue(cfg.rollOffsetRad * kRadToDeg);
    m_autoLevel->setChecked(mode != Mode::Custom || cfg.autoLevel);
    m_customLockDirection->setChecked(mode == Mode::Custom && cfg.lockDirection);
    m_directionSec = cfg.directionSec;
    // Like the horizon amount: reflect the config's value only for the modes
    // that carry it; otherwise sit on the preset default so switching into
    // Horizon lock emits the intended low-pass, not a stale 0.
    m_accelLowPass->setValue(
        (mode == Mode::HorizonLock || mode == Mode::DirectionLock || mode == Mode::Custom)
            ? cfg.accelLowPassHz
            : insv::StabConfig::preset(insv::StabMode::HorizonLock).accelLowPassHz);
    updateModeVisibility();
    m_updating = false;
}

void StabilizationPanel::emitIfEdited()
{
    if (m_updating)
        return;
    const auto mode = Mode(m_mode->currentData().toInt());
    emit modeChanged(mode, nativeConfigForMode());
}

void StabilizationPanel::setDirectionStatus(const QString &text)
{
    m_directionStatus->setText(text);
}

void StabilizationPanel::setSyncStatus(const QString &text)
{
    m_syncStatus->setText(text);
    m_syncStatus->setVisible(!text.isEmpty());
}

void StabilizationPanel::setNoClip()
{
    setState(Mode::Off, insv::StabConfig::preset(insv::StabMode::LeanFollow));
    setBodyEnabled(false);
}

void StabilizationPanel::setCanPaste(bool canPaste)
{
    m_paste->setEnabled(canPaste);
}
