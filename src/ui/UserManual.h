#pragma once

#include <QString>
#include <QUrl>

// Help > User manual: the manual on GitHub, for the version that's running --
// a release ("0.2.0") opens its tag's copy, since main's manual may describe a
// newer UI; a development build ("0.2.0+12.gabc1234", "+dirty") opens main's.
// The app makes no network request itself; the browser does.
QUrl userManualUrl(const QString &version);
