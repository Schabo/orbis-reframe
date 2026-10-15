#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

class QMenuBar;
class QWidget;

// One shortcut made outside the menus, for Help > Keyboard shortcuts. An
// entry with no `what` pairs with the one before it ("Previous / next
// frame": Left / Right); `keysLabel` replaces the keys' own names
// ("Alt+1 … Alt+9").
struct KeyHelp {
    QString group;
    QString what;
    QList<QKeySequence> keys;
    QString keysLabel;
};

// Help > Keyboard shortcuts: `entries` in their groups, then every action
// with a shortcut in `menus`, by menu; in two columns, scrolling when the
// screen is too small.
void showShortcutHelp(QWidget *parent, const QList<KeyHelp> &entries, const QMenuBar *menus);
