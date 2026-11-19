#pragma once

#include "../core/LookPresets.h"

#include <QCoreApplication>
#include <QString>

// The built-in colour looks' names as the user reads them, for the Effects
// library's tiles and the clip's Effects section alike.
namespace looknames {

// The built-in look `presetId` (color::look::kBlackWhite, ...); an id this
// build doesn't know as it is.
inline QString name(const QString &presetId)
{
    if (presetId == QLatin1String(color::look::kBlackWhite))
        return QCoreApplication::translate("looknames", "Black & White");
    if (presetId == QLatin1String(color::look::kDeepSky))
        return QCoreApplication::translate("looknames", "Deep sky");
    if (presetId == QLatin1String(color::look::kTealOrange))
        return QCoreApplication::translate("looknames", "Teal & orange");
    if (presetId == QLatin1String(color::look::kFaded))
        return QCoreApplication::translate("looknames", "Faded film");
    return presetId;
}

} // namespace looknames
