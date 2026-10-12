// The theme (src/ui/Theme): every theme sets every colour token, the
// stylesheet template uses only known tokens, and icons are tinted by mode
// and state. Offscreen platform; exit code is the failure count.

#include "Theme.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

void testTokens()
{
    CHECK(!theme::themes().isEmpty());
    for (const theme::Theme &t : theme::themes()) {
        CHECK(!t.id.isEmpty() && !t.name.isEmpty());
#define ORBIS_CHECK_TOKEN(name, what)                                                              \
    if (!t.colors.name.isValid())                                                                  \
        std::printf("FAIL  theme %s: token %s (%s) not set\n", qPrintable(t.id), #name, what),     \
            ++g_failures;
        ORBIS_THEME_TOKENS(ORBIS_CHECK_TOKEN)
#undef ORBIS_CHECK_TOKEN
    }
}

void testStyleSheet()
{
    for (const theme::Theme &t : theme::themes()) {
        const QString qss = theme::styleSheet(t.colors);
        CHECK(!qss.isEmpty());
        // Every @-name was a token (comments name tokens as "@-name").
        const QRegularExpression left(QStringLiteral("@[A-Za-z]"));
        const auto m = left.match(qss);
        if (m.hasMatch())
            std::printf("FAIL  theme %s: unknown token near \"%s\"\n", qPrintable(t.id),
                        qPrintable(qss.mid(m.capturedStart(), 24)));
        CHECK(!m.hasMatch());
        // Colours come from the tokens, not written into the template.
        CHECK(qss.contains(t.colors.accent.name()));
    }
    CHECK(theme::cssColor(QColor(1, 2, 3)) == "#010203");
    CHECK(theme::cssColor(QColor(0, 0, 0, 77)) == "rgba(0, 0, 0, 77)");
}

// The colour of the most opaque pixel: the icon's stroke.
QColor strokeColor(const QPixmap &pm)
{
    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    QColor best(0, 0, 0, 0);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.alpha() > best.alpha())
                best = c;
        }
    return best;
}

bool near(const QColor &a, const QColor &b)
{
    return std::abs(a.red() - b.red()) <= 2 && std::abs(a.green() - b.green()) <= 2 &&
           std::abs(a.blue() - b.blue()) <= 2;
}

void testIcons()
{
    const theme::Colors &c = theme::colors();
    const QIcon play = theme::icon(QStringLiteral("play"));
    CHECK(!play.isNull());
    CHECK(near(strokeColor(play.pixmap(32, 32, QIcon::Normal)), c.icon));
    CHECK(near(strokeColor(play.pixmap(32, 32, QIcon::Active)), c.iconActive));
    CHECK(near(strokeColor(play.pixmap(32, 32, QIcon::Normal, QIcon::On)), c.iconActive));
    CHECK(near(strokeColor(play.pixmap(32, 32, QIcon::Disabled)), c.textDisabled));
    const QIcon accent = theme::icon(QStringLiteral("play"), theme::IconTone::Accent);
    CHECK(near(strokeColor(accent.pixmap(32, 32, QIcon::Normal)), c.accent));
    // Every bundled icon draws (each says currentColor).
    for (const char *name : {"arrow-left-to-line", "arrow-right-to-line", "camera", "clapperboard",
                             "download", "eye-off", "file-plus-2", "folder-open", "info", "layers",
                             "move-3d", "palette", "pause", "picture-in-picture-2", "rotate-ccw",
                             "save", "trash-2", "vibrate", "volume-2", "x"}) {
        const QColor s = strokeColor(theme::icon(QString::fromLatin1(name)).pixmap(24, 24));
        if (!near(s, c.icon))
            std::printf("FAIL  icon %s isn't drawn in the icon colour\n", name), ++g_failures;
    }
    CHECK(theme::icon(QStringLiteral("no-such-icon")).isNull());
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    theme::apply(app, theme::themes().constFirst());
    CHECK(QApplication::palette().color(QPalette::Highlight) == theme::colors().accent);
    testTokens();
    testStyleSheet();
    testIcons();
    if (g_failures == 0)
        std::printf("theme_test: all checks passed\n");
    else
        std::printf("theme_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
