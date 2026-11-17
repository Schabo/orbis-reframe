#pragma once

#include <QKeyEvent>
#include <QListWidget>

// A list of things to add to the edit -- a title, a gauge, a transition, an
// effect -- where adding is the deliberate gesture: a double-click on a tile,
// or Return / Enter on the current one. Not Qt's activation (itemActivated):
// a desktop set to open files and folders on a single click reports a click as
// an activation, and then a click meant to select would add something. Only
// the panels' add actions answer that; a click here selects.
class DoubleClickToAddList : public QListWidget {
    Q_OBJECT

public:
    using QListWidget::QListWidget;

signals:
    // Return or Enter, with an item current.
    void activationKeyPressed();

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            && e->modifiers() == Qt::NoModifier && currentItem()) {
            emit activationKeyPressed();
            e->accept();
            return;
        }
        QListWidget::keyPressEvent(e);
    }
};
