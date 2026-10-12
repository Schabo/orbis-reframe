#include "TabSidebar.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "Theme.h"

namespace {

QToolButton *iconButton(const QString &iconName, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setAutoRaise(true);
    b->setIcon(theme::icon(iconName));
    b->setIconSize(QSize(18, 18));
    b->setFocusPolicy(Qt::NoFocus); // the editor's keys stay with the editor
    return b;
}

} // namespace

// The drag handle on the sidebar's inner edge.
class TabSidebarHandle : public QWidget {
public:
    explicit TabSidebarHandle(TabSidebar *bar) : QWidget(bar), m_bar(bar)
    {
        setFixedWidth(5);
        setCursor(Qt::SizeHorCursor);
        setAttribute(Qt::WA_StyledBackground);
        setAttribute(Qt::WA_Hover);
        setToolTip(TabSidebar::tr("Drag to resize; double-click for the default width"));
    }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_dragging = true;
        m_pressX = e->globalPosition().x();
        m_pressWidth = m_bar->pageWidth();
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!m_dragging)
            return;
        const double dx = e->globalPosition().x() - m_pressX;
        // The left sidebar grows to the right, the right one to the left.
        const int width = int(m_pressWidth + (m_bar->m_side == TabSidebar::Side::Left ? dx : -dx));
        m_bar->m_panel->setFixedWidth(m_bar->clampedWidth(width));
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !m_dragging)
            return;
        m_dragging = false;
        if (m_bar->pageWidth() != m_pressWidth)
            emit m_bar->pageWidthChanged(m_bar->pageWidth());
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_bar->setPageWidth(m_bar->m_standardWidth);
        emit m_bar->pageWidthChanged(m_bar->pageWidth());
    }

private:
    TabSidebar *m_bar;
    bool m_dragging = false;
    double m_pressX = 0;
    int m_pressWidth = 0;
};

TabSidebar::TabSidebar(Side side, const QString &objectName, QWidget *parent)
    : QWidget(parent), m_side(side)
{
    setObjectName(objectName);
    setProperty("sidebarSide", side == Side::Left ? "left" : "right");
    // A QWidget subclass paints its stylesheet background only when asked.
    setAttribute(Qt::WA_StyledBackground);

    const bool left = side == Side::Left;
    const QString closeIcon = left ? QStringLiteral("chevron-left") : QStringLiteral("chevron-right");
    const QString openIcon = left ? QStringLiteral("chevron-right") : QStringLiteral("chevron-left");

    // Open: the tab row over the pages.
    m_panel = new QWidget(this);
    m_tabRow = new QWidget(m_panel);
    m_tabRow->setProperty("sidebarPart", "tabRow");
    m_tabRow->setAttribute(Qt::WA_StyledBackground);
    m_tabLayout = new QHBoxLayout(m_tabRow);
    m_tabLayout->setContentsMargins(2, 2, 2, 0);
    m_tabLayout->setSpacing(2);
    m_close = iconButton(closeIcon, m_tabRow);
    m_close->setObjectName(objectName + QStringLiteral("Close"));
    m_close->setProperty("sidebarPart", "close");
    connect(m_close, &QToolButton::clicked, this, [this] { setOpen(false); });
    // The close button at the row's outer end; tabs fill in from the other.
    if (left) {
        m_tabLayout->addWidget(m_close);
        m_tabLayout->addStretch(1);
    } else {
        m_tabLayout->addStretch(1);
        m_tabLayout->addWidget(m_close);
    }

    m_tabs = new QButtonGroup(this);
    m_tabs->setExclusive(true);
    m_stack = new QStackedWidget(m_panel);
    connect(m_tabs, &QButtonGroup::idClicked, this, &TabSidebar::setCurrentIndex);

    auto *panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);
    panelLayout->addWidget(m_tabRow);
    panelLayout->addWidget(m_stack, 1);

    // Closed: a slim strip at the edge.
    m_strip = new QWidget(this);
    m_strip->setObjectName(objectName + QStringLiteral("Strip"));
    m_strip->setProperty("sidebarPart", "strip");
    m_strip->setAttribute(Qt::WA_StyledBackground);
    m_reopen = iconButton(openIcon, m_strip);
    m_reopen->setObjectName(objectName + QStringLiteral("Open"));
    m_reopen->setIconSize(QSize(14, 14));
    connect(m_reopen, &QToolButton::clicked, this, [this] { setOpen(true); });
    auto *stripLayout = new QVBoxLayout(m_strip);
    stripLayout->setContentsMargins(0, 2, 0, 2);
    stripLayout->addWidget(m_reopen, 0, Qt::AlignHCenter);
    stripLayout->addStretch(1);
    m_strip->hide();

    m_handle = new TabSidebarHandle(this);
    m_handle->setObjectName(objectName + QStringLiteral("Handle"));
    m_handle->setProperty("sidebarPart", "handle");

    // The handle on the inner edge, toward the viewport.
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (left) {
        layout->addWidget(m_panel, 1);
        layout->addWidget(m_handle, 0);
    } else {
        layout->addWidget(m_handle, 0);
        layout->addWidget(m_panel, 1);
    }
    layout->addWidget(m_strip, 0);
    updateTooltips();
}

void TabSidebar::addTab(QWidget *page, const QString &iconName, const QString &title)
{
    const int index = m_stack->addWidget(page);
    QToolButton *b = iconButton(iconName, m_tabRow);
    b->setCheckable(true);
    b->setObjectName(page->objectName() + QStringLiteral("Tab"));
    b->setProperty("sidebarPart", "tab");
    b->setToolTip(title);
    b->setAccessibleName(title);
    m_tabs->addButton(b, index);
    // Tabs in the order added, next to the close button on the left side,
    // from the left on the right side.
    const int at = m_side == Side::Left ? 1 + index : index;
    m_tabLayout->insertWidget(at, b);
    if (index == 0)
        setCurrentIndex(0);
}

void TabSidebar::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_stack->count())
        return;
    m_stack->setCurrentIndex(index);
    if (QAbstractButton *b = m_tabs->button(index))
        b->setChecked(true);
}

int TabSidebar::currentIndex() const { return m_stack->currentIndex(); }

int TabSidebar::count() const { return m_stack->count(); }

void TabSidebar::setPageWidthRange(int min, int standard, int max)
{
    m_minWidth = min;
    m_maxWidth = std::max(min, max);
    m_standardWidth = std::clamp(standard, m_minWidth, m_maxWidth);
}

void TabSidebar::setPageWidth(int width)
{
    m_panel->setFixedWidth(std::clamp(width, m_minWidth, m_maxWidth));
}

int TabSidebar::pageWidth() const { return m_panel->minimumWidth(); } // a fixed width

int TabSidebar::clampedWidth(int width) const
{
    int max = m_maxWidth;
    if (const QWidget *w = window(); w && w != this)
        max = std::min(max, w->width() / 2);
    return std::clamp(width, m_minWidth, std::max(m_minWidth, max));
}

void TabSidebar::setOpen(bool open)
{
    if (open == m_open)
        return;
    m_open = open;
    m_panel->setVisible(open);
    m_handle->setVisible(open);
    m_strip->setVisible(!open);
    emit openChanged(open);
}

void TabSidebar::setToggleKeys(const QString &keys)
{
    m_toggleKeys = keys;
    updateTooltips();
}

void TabSidebar::updateTooltips()
{
    const QString keys =
        m_toggleKeys.isEmpty() ? QString() : QStringLiteral("  ( %1 )").arg(m_toggleKeys);
    m_close->setToolTip(tr("Close the sidebar") + keys);
    m_reopen->setToolTip(tr("Open the sidebar") + keys);
}
