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

    // Return, or the keypad's Enter, with no other modifier: the key that adds.
    // The keypad's key always carries Qt::KeypadModifier, so that one is
    // ignored.
    static bool isActivationKey(const QKeyEvent *e)
    {
        return (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
               && (e->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    }

signals:
    // Return or Enter, with an item current.
    void activationKeyPressed();

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (isActivationKey(e) && currentItem()) {
            emit activationKeyPressed();
            e->accept();
            return;
        }
        QListWidget::keyPressEvent(e);
    }
};
