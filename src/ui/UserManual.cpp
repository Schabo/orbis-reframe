#include "UserManual.h"

QUrl userManualUrl(const QString &version)
{
    // Build metadata after a '+' (cmake/GitVersion.cmake) means not a release.
    const bool release = !version.isEmpty() && !version.contains(QLatin1Char('+'));
    const QString ref = release ? QStringLiteral("v") + version : QStringLiteral("main");
    return QUrl(QStringLiteral("https://github.com/Schabo/orbis-reframe/blob/%1/docs/manual/README.md")
                    .arg(ref));
}
