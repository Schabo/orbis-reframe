#pragma once

#include <QWidget>

class QButtonGroup;
class QStackedWidget;
class QVBoxLayout;
class SettingsPanel;

// The right-hand settings dock: a vertical icon rail on the far edge plus a
// one-at-a-time stack of SettingsPanels. Clicking a rail icon shows that
// panel; clicking the active icon again collapses the stack (the rail stays).
// MainWindow adds the panels and wires their controls to the model.
class SettingsSidebar : public QWidget {
    Q_OBJECT

public:
    explicit SettingsSidebar(QWidget *parent = nullptr);

    // Takes ownership of `panel`, adds a rail button using its icon + title.
    void addPanel(SettingsPanel *panel);
    // Bring `panel` to the front, unless the stack is collapsed (then it
    // stays collapsed: the user put it away).
    void raisePanel(SettingsPanel *panel);
    // What clicking the index'th rail icon does (Alt+1 … Alt+9): show that
    // panel, or collapse the stack if it's the one showing.
    void togglePanel(int index);

private:
    void showPanel(int index);
    void setCollapsed(bool collapsed);

    QStackedWidget *m_stack = nullptr;
    QWidget *m_rail = nullptr;
    QVBoxLayout *m_railLayout = nullptr;
    QButtonGroup *m_railGroup = nullptr;
    int m_currentIndex = -1;
    bool m_collapsed = false;
};
