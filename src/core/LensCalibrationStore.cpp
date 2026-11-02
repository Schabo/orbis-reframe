#include "LensCalibrationStore.h"

#include "LensCalibrationJson.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

#include <cctype>

namespace lenscal {

namespace {

QString FilePath(const std::string& serial, int lensGuard)
{
    // The serial as it is, less anything a file name can't hold.
    QString s;
    for (const char ch : serial)
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')
            s += QLatin1Char(ch);
    if (s.isEmpty())
        return {};
    return StoreDir() + QLatin1Char('/') + s + QLatin1Char('-') +
           QString::fromLatin1(GuardName(lensGuard)) + QStringLiteral(".json");
}

} // namespace

QString StoreDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/calibrations");
}

std::optional<stitch::LensCalibration> Load(const std::string& serial, int lensGuard)
{
    const QString path = FilePath(serial, lensGuard);
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const nlohmann::json j = nlohmann::json::parse(f.readAll().toStdString(), nullptr, false);
    auto c = FromJson(j);
    if (!c || c->serial != serial || c->lensGuard != lensGuard)
        return std::nullopt;
    return c;
}

bool Save(const stitch::LensCalibration& c, QString* error)
{
    const QString path = FilePath(c.serial, c.lensGuard);
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("the camera has no serial number");
        return false;
    }
    QDir().mkpath(StoreDir());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(QByteArray::fromStdString(ToJson(c).dump(2) + "\n"));
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

std::optional<stitch::LensCalibration> ForClip(const std::string& serial, int lensGuard,
                                               const insv::LensCalibExt& lens0,
                                               const insv::LensCalibExt& lens1)
{
    auto c = Load(serial, lensGuard);
    if (c && !c->matchesFile(lens0, lens1))
        return std::nullopt;
    return c;
}

} // namespace lenscal
