#pragma once

#include "../core/Gauge.h"
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

// The title preset `p` adds: its style in the system's font and units
// (title::defaultUnits). A date, a clock or a GPS value is a field in its
// text (title::withFields).
title::Style styleFor(const Preset &p);

// The ready-made gauges, the Overlays tab's second group: a kind of gauge
// reading a value, and a place in the picture. Like a title's tile, only
// where a gauge starts.
struct GaugePreset {
    QString id; // drag data, as a title preset's; never one of theirs
    QString name;
    QString toolTip;
    gauge::Style style; // units and range: the system's (gaugeStyleFor)
    inset::Look look;   // width = the gauge's height (gauge::Style)
};

const QList<GaugePreset> &gauges();
// The gauge preset `id` names, or null.
const GaugePreset *findGauge(const QString &id);
// The gauge preset `p` adds: in the system's font and units, its scale the
// one that suits them (gauge::defaultRange).
gauge::Style gaugeStyleFor(const GaugePreset &p);

} // namespace titlepresets
