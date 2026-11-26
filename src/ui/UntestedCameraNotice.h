#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

class QWidget;

// The notice for clips from a camera orbis-reframe opens but hasn't been
// tested with (camera::ClipMetadata::untestedModel): they open on general
// values, not ones measured on that model, so a seam or horizon that's off
// is the missing measurements, not the app at its best -- and sample clips
// fix it. Shown when such a clip is added, once per model while the app
// runs, with "Don't show this again" (per model, stored in
// ~/.config/orbis-reframe/orbis-reframe.conf).

namespace untestedcamera {

// Where the notice asks for sample clips: the format docs' list on main,
// which says what's wanted now, whatever version is running. The app makes
// no network request itself; the browser does.
QUrl sampleClipsUrl();

// Of `models` (as the files name them), the ones to tell about, each once
// and in order: not in `hidden` (Don't show this again) and not in `told`
// (this run's), which this adds them to.
QStringList toTell(const QStringList &models, const QStringList &hidden, QSet<QString> &told);

// The notice's text, rich text with the sample-clips link, for `models`
// (one or more).
QString noticeText(const QStringList &models);

// Tells the user about the ones of `models` that toTell() picks, in one
// dialog over `parent`; nothing when none.
void tell(QWidget *parent, const QStringList &models);

} // namespace untestedcamera
