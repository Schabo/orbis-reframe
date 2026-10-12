// Help > User manual's link: a release opens its tag's manual, a development
// build main's. Exit code is the failure count.

#include "UserManual.h"

#include <cstdio>

namespace {

int g_failures = 0;

void expect(const QString &version, const char *ref)
{
    const QString want =
        QStringLiteral("https://github.com/Schabo/orbis-reframe/blob/%1/docs/manual/README.md")
            .arg(QLatin1String(ref));
    const QString got = userManualUrl(version).toString();
    if (got != want) {
        std::printf("FAIL  \"%s\": got %s, want %s\n", qPrintable(version), qPrintable(got),
                    qPrintable(want));
        ++g_failures;
    }
}

} // namespace

int main()
{
    expect(QStringLiteral("0.2.0"), "v0.2.0");
    expect(QStringLiteral("1.10.3"), "v1.10.3");
    expect(QStringLiteral("0.2.0+12.gabc1234"), "main");
    expect(QStringLiteral("0.2.0+12.gabc1234.dirty"), "main");
    expect(QStringLiteral("0.2.0+dirty"), "main");
    expect(QStringLiteral("0.0.0+gabc1234"), "main");
    expect(QString(), "main");
    if (g_failures == 0)
        std::printf("user_manual_test: all checks passed\n");
    else
        std::printf("user_manual_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
