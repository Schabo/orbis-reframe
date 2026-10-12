#pragma once

#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QStackedWidget;
class QToolButton;

// A sidebar at one edge of the editor: a row of icon tabs along its top (the
// tooltip names each), one page at a time below them, and a close button at
// the row's outer end (`<` on the left side, `>` on the right). Closed, it
// leaves a slim strip at the edge with a button to open it again. Open, a
// handle on its inner edge (toward the viewport) drags it wider or narrower;
// a double-click on the handle goes back to the default width.
//
// Object names, from the one given: <name> itself, <name>Close (the close
// button), <name>Handle (the resize handle), <name>Strip and <name>Open (the
// strip and its button), and <page's objectName>Tab for each tab button.
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
    // What a tab's key does (Alt+1 …): show that tab, opening the sidebar if
    // it's closed; the tab already showing closes the sidebar instead.
    void toggleTab(int index);
    int count() const;

    // The page area's width (the tab row and pages; not the strip). The
    // handle drags it between `min` and `max`, and to at most half the
    // window; a double-click on it goes back to `standard`. setPageWidth
    // clamps to the range too.
    void setPageWidthRange(int min, int standard, int max);
    void setPageWidth(int width);
    int pageWidth() const;

    bool isOpen() const { return m_open; }
    void setOpen(bool open);
    void toggle() { setOpen(!m_open); }
    // The open / close shortcut, named in the buttons' tooltips.
    void setToggleKeys(const QString &keys);

signals:
    void openChanged(bool open);
    void currentChanged(int index);
    // A drag on the handle (or a double-click) ended at this width.
    void pageWidthChanged(int width);

private:
    friend class TabSidebarHandle;
    void updateTooltips();
    // The width a handle drag may set: the range, and at most half the
    // window.
    int clampedWidth(int width) const;

    Side m_side;
    bool m_open = true;
    QString m_toggleKeys;
    QWidget *m_panel = nullptr;
    QWidget *m_tabRow = nullptr;
    QHBoxLayout *m_tabLayout = nullptr;
    QButtonGroup *m_tabs = nullptr;
    QStackedWidget *m_stack = nullptr;
    QToolButton *m_close = nullptr;
    QWidget *m_handle = nullptr;
    int m_minWidth = 200;
    int m_standardWidth = 300;
    int m_maxWidth = 800;
    QWidget *m_strip = nullptr;
    QToolButton *m_reopen = nullptr;
};
