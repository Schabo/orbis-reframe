#pragma once

#include "../core/Title.h"

#include <QList>
#include <QString>

// The ready-made titles the Overlays tab offers as tiles: where a title
// starts, a style, a place in the picture and how it comes and goes. Nothing about the tile is
// kept -- once added it is an ordinary title, edited like any other.
namespace titlepresets {

struct Preset {
    QString id;      // the tile's drag data; stays the same across versions
    QString name;    // the tile's label
    QString toolTip; // what it is for
    title::Style style; // its text is the placeholder; font: the system's
    inset::Look look;   // width = the text size (title::drawnLook)
    inset::Motion motion; // how it comes on and goes off
};

// In the order the tab shows them, the plain one first.
const QList<Preset> &all();
// The preset `id` names, or null.
const Preset *find(const QString &id);

// The title preset `p` adds: its style in the system's font. A date or a
// clock is a field in its text (title::withFields).
title::Style styleFor(const Preset &p);

} // namespace titlepresets
