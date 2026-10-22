#include "ReframePanel.h"

#include "EaseCombo.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

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
    // Split screen, above the view fields: they steer the pane picked here.
    m_layout = new QComboBox(this);
    m_layout->setObjectName(QStringLiteral("splitLayoutCombo"));
    m_layout->addItem(tr("One picture"), int(pane::Layout::Single));
    m_layout->addItem(tr("Side by side"), int(pane::Layout::SideBySide));
    m_layout->addItem(tr("Stacked"), int(pane::Layout::Stacked));
    m_layout->addItem(tr("Grid (2 × 2)"), int(pane::Layout::Grid));
    m_layout->setToolTip(tr("Split this clip's picture into panes, each looking its own way "
                            "with its own keyframes."));
    form()->addRow(tr("Split screen"), m_layout);
    m_pane = new QComboBox(this);
    m_pane->setObjectName(QStringLiteral("activePaneCombo"));
    m_pane->setToolTip(tr("The pane the view fields, a drag in the view and new keyframes "
                          "steer. Clicking a pane in the view picks it too."));
    form()->addRow(tr("Pane"), m_pane);
    m_gap = new QDoubleSpinBox(this);
    m_gap->setObjectName(QStringLiteral("splitGapBox"));
    m_gap->setRange(0.0, pane::Split::kMaxGap * 100.0);
    m_gap->setDecimals(1);
    m_gap->setSingleStep(0.1);
    m_gap->setSuffix(QStringLiteral(" %"));
    m_gap->setKeyboardTracking(false);
    m_gap->setToolTip(tr("The divider between the panes, as a share of the picture's height "
                         "(0.4 % is about 4 pixels at 1080p); 0 lets the panes touch."));
    m_gapColor = new QPushButton(this);
    m_gapColor->setObjectName(QStringLiteral("splitGapColorButton"));
    m_gapColor->setToolTip(tr("The divider's colour."));
    auto *gapRow = new QWidget(this);
    auto *gapLayout = new QHBoxLayout(gapRow);
    gapLayout->setContentsMargins(0, 0, 0, 0);
    gapLayout->addWidget(m_gap, 1);
    gapLayout->addWidget(m_gapColor);
    form()->addRow(tr("Divider"), gapRow);
    m_fillPanes = new QPushButton(tr("Fill panes"), this);
    m_fillPanes->setObjectName(QStringLiteral("fillPanesButton"));
    m_fillPanes->setToolTip(tr("Point the panes front and back (two panes), or front, right, "
                               "left and back (the grid), from the first pane's view: a "
                               "keyframe in each pane at the playhead."));
    form()->addRow(m_fillPanes);
    connect(m_layout, &QComboBox::activated, this, [this] {
        showSplitRows();
        emit splitEdited(currentSplit());
    });
    connect(m_gap, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!m_updating)
            emit splitEdited(currentSplit());
    });
    connect(m_gapColor, &QPushButton::clicked, this, [this] {
        const QColor c =
            QColorDialog::getColor(QColor::fromRgb(m_gapRgb), this, tr("Divider colour"));
        if (!c.isValid())
            return;
        setGapColor(c.rgb() & 0xffffff);
        emit splitEdited(currentSplit());
    });
    connect(m_pane, &QComboBox::activated, this, [this](int i) { emit activePaneEdited(i); });
    connect(m_fillPanes, &QPushButton::clicked, this, &ReframePanel::fillPanesRequested);

    m_yaw = makeAngleBox(this, -180.0, 180.0, true);
    m_pitch = makeAngleBox(this, -89.0, 89.0, false);
    m_roll = makeAngleBox(this, -180.0, 180.0, true);
    m_fov = makeAngleBox(this, 20.0, 110.0, false);

    form()->addRow(tr("Yaw"), m_yaw);
    form()->addRow(tr("Pitch"), m_pitch);
    form()->addRow(tr("Roll"), m_roll);
    form()->addRow(tr("Field of view"), m_fov);

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
    setSplit(pane::Split{});

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

pane::Split ReframePanel::currentSplit() const
{
    pane::Split s;
    s.layout = static_cast<pane::Layout>(m_layout->currentData().toInt());
    s.gap = static_cast<float>(m_gap->value() / 100.0);
    s.gapRgb = m_gapRgb;
    return s;
}

void ReframePanel::setSplit(const pane::Split &split)
{
    m_updating = true;
    m_layout->setCurrentIndex(std::max(0, m_layout->findData(int(split.layout))));
    m_gap->setValue(split.gap * 100.0);
    setGapColor(split.gapRgb);
    m_updating = false;
    showSplitRows();
}

void ReframePanel::setActivePane(int pane)
{
    m_pane->setCurrentIndex(std::clamp(pane, 0, std::max(0, m_pane->count() - 1)));
}

void ReframePanel::setGapColor(uint32_t rgb)
{
    m_gapRgb = rgb & 0xffffff;
    const QColor c = QColor::fromRgb(m_gapRgb);
    m_gapColor->setText(c.name());
    m_gapColor->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; }")
            .arg(c.name(), c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

void ReframePanel::showSplitRows()
{
    const pane::Layout l = currentSplit().layout;
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
    form()->setRowVisible(m_layout, !m_flat);
    form()->setRowVisible(m_pane, !m_flat && split);
    form()->setRowVisible(m_gap->parentWidget(), !m_flat && split);
    form()->setRowVisible(m_fillPanes, !m_flat && split);
}

void ReframePanel::setClipUnkeyed(bool unkeyed)
{
    m_unkeyed->setText(currentSplit().layout == pane::Layout::Single
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
         std::initializer_list<QWidget *>{m_yaw, m_pitch, m_roll, m_fov, m_ease, m_track})
        form()->setRowVisible(w, !flat);
    form()->setRowVisible(m_trackUnavailable, !flat && !m_trackUnavailableReason.isEmpty());
    form()->setRowVisible(m_trackStatus, !flat && m_trackRunning);
    if (flat)
        m_unkeyed->hide();
    form()->setRowVisible(m_fit, flat);
    m_fit->setCurrentIndex(fill ? 1 : 0);
    showSplitRows();
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
    for (QWidget *w : std::initializer_list<QWidget *>{m_yaw, m_pitch, m_roll, m_fov, m_layout,
                                                       m_pane, m_gap, m_gapColor, m_fillPanes})
        w->setEnabled(!running);
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
