#pragma once

#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QStackedWidget;
class QToolButton;

// A sidebar at one edge of the editor: a row of icon tabs along its top (the
// tooltip names each), one page at a time below them, and a close button at
// the row's outer end (`<` on the left side, `>` on the right). Closed, it
// leaves a slim strip at the edge with a button to open it again.
//
// Object names, from the one given: <name> itself, <name>Close (the close
// button), <name>Strip and <name>Open (the strip and its button), and
// <page's objectName>Tab for each tab button.
class TabSidebar : public QWidget {
    Q_OBJECT

public:
    enum class Side { Left, Right };

    TabSidebar(Side side, const QString &objectName, QWidget *parent = nullptr);

    // Takes ownership of `page`; its tab shows the Lucide icon `iconName`.
    // `title` is its tooltip.
    void addTab(QWidget *page, const QString &iconName, const QString &title);
    void setCurrentIndex(int index);
    int currentIndex() const;
    int count() const;

    // The page area's width (the tab row and pages; not the strip).
    void setPageWidth(int width);

    bool isOpen() const { return m_open; }
    void setOpen(bool open);
    void toggle() { setOpen(!m_open); }
    // The open / close shortcut, named in the buttons' tooltips.
    void setToggleKeys(const QString &keys);

signals:
    void openChanged(bool open);

private:
    void updateTooltips();

    Side m_side;
    bool m_open = true;
    QString m_toggleKeys;
    QWidget *m_panel = nullptr;
    QWidget *m_tabRow = nullptr;
    QHBoxLayout *m_tabLayout = nullptr;
    QButtonGroup *m_tabs = nullptr;
    QStackedWidget *m_stack = nullptr;
    QToolButton *m_close = nullptr;
    QWidget *m_strip = nullptr;
    QToolButton *m_reopen = nullptr;
};
