#include "SplitScreenPanel.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>

#include <algorithm>

SplitScreenPanel::SplitScreenPanel(QWidget *parent)
    : SettingsPanel(tr("Split screen"), parent)
{
    m_layout = new QComboBox(this);
    m_layout->setObjectName(QStringLiteral("splitLayoutCombo"));
    m_layout->addItem(tr("One picture"), int(pane::Layout::Single));
    m_layout->addItem(tr("Side by side"), int(pane::Layout::SideBySide));
    m_layout->addItem(tr("Stacked"), int(pane::Layout::Stacked));
    m_layout->addItem(tr("Grid (2 × 2)"), int(pane::Layout::Grid));
    m_layout->setToolTip(tr("Split this clip's picture into panes, each looking its own way "
                            "with its own keyframes."));
    form()->addRow(tr("Layout"), m_layout);
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
    connect(m_fillPanes, &QPushButton::clicked, this, &SplitScreenPanel::fillPanesRequested);

    setSplit(pane::Split{});
}

pane::Split SplitScreenPanel::currentSplit() const
{
    pane::Split s;
    s.layout = static_cast<pane::Layout>(m_layout->currentData().toInt());
    s.gap = static_cast<float>(m_gap->value() / 100.0);
    s.gapRgb = m_gapRgb;
    return s;
}

void SplitScreenPanel::setSplit(const pane::Split &split)
{
    m_updating = true;
    m_layout->setCurrentIndex(std::max(0, m_layout->findData(int(split.layout))));
    m_gap->setValue(split.gap * 100.0);
    setGapColor(split.gapRgb);
    m_updating = false;
    showSplitRows();
    setBodyEnabled(true);
}

void SplitScreenPanel::setNoClip()
{
    setSplit(pane::Split{});
    setBodyEnabled(false);
}

void SplitScreenPanel::setEditable(bool editable)
{
    for (QWidget *w : std::initializer_list<QWidget *>{m_layout, m_gap, m_gapColor, m_fillPanes})
        w->setEnabled(editable);
}

void SplitScreenPanel::setGapColor(uint32_t rgb)
{
    m_gapRgb = rgb & 0xffffff;
    const QColor c = QColor::fromRgb(m_gapRgb);
    m_gapColor->setText(c.name());
    m_gapColor->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; }")
            .arg(c.name(), c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

void SplitScreenPanel::showSplitRows()
{
    const bool split = currentSplit().layout != pane::Layout::Single;
    form()->setRowVisible(m_gap->parentWidget(), split);
    form()->setRowVisible(m_fillPanes, split);
    // Only a split clip has panes to pick.
    setHint(split ? tr("Pick the pane to steer in the Reframe section, or click it in the "
                       "view.")
                  : QString());
}
