#pragma once

#include "../../core/KeyTrack.h" // keytrack::Ease

#include <QComboBox>
#include <QCoreApplication>
#include <QSignalBlocker>

#include <algorithm>

// The Easing field the Reframe and Overlay panels share: how a keyframed value
// moves from the key at (or before) the playhead to the next one. Items in
// keytrack::Ease order; the item's index is the Ease.
namespace easecombo {

inline QComboBox *make(QWidget *parent)
{
    auto *c = new QComboBox(parent);
    c->addItem(QCoreApplication::translate("EaseCombo", "Linear"));
    c->addItem(QCoreApplication::translate("EaseCombo", "Fast to slow"));
    c->addItem(QCoreApplication::translate("EaseCombo", "Slow to fast"));
    return c;
}

inline keytrack::Ease ease(const QComboBox *c)
{
    return static_cast<keytrack::Ease>(std::max(0, c->currentIndex()));
}

// Show the segment at the playhead's ease, or grey the field out when the
// playhead isn't between two keys (`available` false). Doesn't emit.
inline void show(QComboBox *c, bool available, keytrack::Ease e)
{
    const QSignalBlocker block(c);
    c->setCurrentIndex(available ? static_cast<int>(e) : 0);
    c->setEnabled(available);
}

} // namespace easecombo
