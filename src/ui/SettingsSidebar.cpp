#include "SettingsSidebar.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QIcon>
#include <QSize>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "SettingsPanel.h"

SettingsSidebar::SettingsSidebar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("settingsSidebar"));

    // A fixed width: the panels' preferred widths follow what they show (a
    // file name, a word-wrapped note, a stream list), and a sidebar sized
    // from them jumps whenever the timeline selection changes. Scaled with
    // the font; addPanel() widens it if a panel's minimum needs more.
    m_stack = new QStackedWidget(this);
    m_stack->setFixedWidth(std::max(340, fontMetrics().height() * 20));

    m_rail = new QWidget(this);
    m_rail->setObjectName(QStringLiteral("settingsRail"));
    m_railLayout = new QVBoxLayout(m_rail);
    m_railLayout->setContentsMargins(0, 6, 0, 6);
    m_railLayout->setSpacing(2);
    m_railLayout->addStretch(1);

    m_railGroup = new QButtonGroup(this);
    m_railGroup->setExclusive(true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_rail, 0);
}

void SettingsSidebar::addPanel(SettingsPanel *panel)
{
    const int index = m_stack->addWidget(panel);
    if (panel->minimumSizeHint().width() > m_stack->maximumWidth())
        m_stack->setFixedWidth(panel->minimumSizeHint().width());

    auto *button = new QToolButton(m_rail);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setIcon(QIcon(panel->iconPath()));
    button->setIconSize(QSize(20, 20));
    // MainWindow gives the first nine panels Alt+1 … Alt+9.
    button->setToolTip(index < 9 ? tr("%1  ( Alt+%2 )").arg(panel->title()).arg(index + 1)
                                 : panel->title());
    // Insert before the trailing stretch.
    m_railLayout->insertWidget(m_railLayout->count() - 1, button);
    m_railGroup->addButton(button, index);

    connect(button, &QToolButton::clicked, this, [this, index] { togglePanel(index); });

    if (m_currentIndex < 0)
        showPanel(index);
}

void SettingsSidebar::raisePanel(SettingsPanel *panel)
{
    const int index = m_stack->indexOf(panel);
    if (index >= 0 && !m_collapsed && index != m_currentIndex)
        showPanel(index);
}

void SettingsSidebar::togglePanel(int index)
{
    if (index < 0 || index >= m_stack->count())
        return;
    if (index == m_currentIndex)
        setCollapsed(!m_collapsed);
    else
        showPanel(index);
}

void SettingsSidebar::showPanel(int index)
{
    m_currentIndex = index;
    m_stack->setCurrentIndex(index);
    if (auto *b = m_railGroup->button(index))
        b->setChecked(true);
    setCollapsed(false);
}

void SettingsSidebar::setCollapsed(bool collapsed)
{
    m_collapsed = collapsed;
    m_stack->setVisible(!collapsed);
    if (collapsed) {
        if (auto *b = m_railGroup->button(m_currentIndex))
            b->setChecked(false);
    } else if (auto *b = m_railGroup->button(m_currentIndex)) {
        b->setChecked(true);
    }
}
