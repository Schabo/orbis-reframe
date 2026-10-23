// The theme (src/ui/Theme): every theme sets every colour token, its text
// reads on its surfaces, the stylesheet template uses only known tokens,
// icons are tinted by mode and state, and switching themes re-tints them and
// says so. Offscreen platform; exit code is the failure count.

#include "Theme.h"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
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
        // Icon urls name files holding the icon in the token's colour.
        CHECK(!qss.contains(QStringLiteral("url(icon:")));
        const QRegularExpression arrow(QStringLiteral("down-arrow \\{[^}]*url\\(([^)]+)\\)"));
        const auto a = arrow.match(qss);
        CHECK(a.hasMatch());
        QFile file(a.captured(1));
        CHECK(file.open(QIODevice::ReadOnly));
        const QByteArray svg = file.readAll();
        CHECK(svg.contains(t.colors.icon.name().toLatin1()) && !svg.contains("currentColor"));
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

// Without Qt's SVG image plugin no icon draws at all: main() says so once
// instead of a wrong colour per icon.
bool canDrawIcons() { return QImageReader::supportedImageFormats().contains("svg"); }

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
                             "download", "upload", "eye-off", "file-plus-2", "folder-open", "info", "layers",
                             "move-3d", "palette", "pause", "picture-in-picture-2", "rotate-ccw",
                             "save", "trash-2", "vibrate", "volume-2", "x"}) {
        const QColor s = strokeColor(theme::icon(QString::fromLatin1(name)).pixmap(24, 24));
        if (!near(s, c.icon))
            std::printf("FAIL  icon %s isn't drawn in the icon colour\n", name), ++g_failures;
    }
    CHECK(theme::icon(QStringLiteral("no-such-icon")).isNull());
}

// WCAG contrast ratio of two opaque colours.
double contrast(const QColor &a, const QColor &b)
{
    const auto lum = [](const QColor &c) {
        const auto ch = [](double v) {
            return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
    };
    const double la = lum(a), lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

void testContrast()
{
    for (const theme::Theme &t : theme::themes()) {
        const theme::Colors &c = t.colors;
        const auto atLeast = [&](const QColor &fg, const QColor &bg, double min, const char *what) {
            const double r = contrast(fg, bg);
            if (r < min)
                std::printf("FAIL  theme %s: %s contrast %.2f < %.1f\n", qPrintable(t.id), what, r,
                            min),
                    ++g_failures;
        };
        for (const QColor &bg : {c.window, c.panel, c.button}) {
            atLeast(c.text, bg, 7.0, "text");
            atLeast(c.textDim, bg, 4.5, "dim text");
            atLeast(c.icon, bg, 3.0, "icon");
            atLeast(c.accent, bg, 3.0, "accent");
        }
        atLeast(c.textOnAccent, c.accent, 3.0, "text on accent");
        atLeast(c.text, c.clipVideo, 3.0, "clip name on a video clip");
        atLeast(c.text, c.clipAudio, 3.0, "clip name on an audio clip");
        atLeast(c.error, c.panel, 3.0, "error");
        atLeast(c.warning, c.panel, 3.0, "warning");
    }
}

// Switching: current() follows, the palette and stylesheet change, icons
// re-tint on their next draw, changed() fires; an unknown id is the default.
void testSwitch(QApplication &app)
{
    CHECK(theme::themes().size() >= 3);
    CHECK(theme::byId(QStringLiteral("nope")).id == theme::themes().constFirst().id);
    CHECK(theme::byId(QString()).id == theme::themes().constFirst().id);
    int changed = 0;
    QObject::connect(theme::notifier(), &theme::Notifier::changed, [&changed] { ++changed; });
    const QIcon play = theme::icon(QStringLiteral("play"));
    for (const theme::Theme &t : theme::themes()) {
        theme::apply(app, t);
        CHECK(theme::current().id == t.id);
        CHECK(QApplication::palette().color(QPalette::Window) == t.colors.window);
        CHECK(app.styleSheet().contains(t.colors.accent.name()));
        if (canDrawIcons())
            CHECK(near(strokeColor(play.pixmap(32, 32, QIcon::Normal)), t.colors.icon));
    }
    CHECK(changed == int(theme::themes().size()));
    CHECK(theme::byId(QStringLiteral("light")).id == QStringLiteral("light"));
    CHECK(theme::byId(QStringLiteral("neutral-dark")).id == QStringLiteral("neutral-dark"));
    theme::apply(app, theme::themes().constFirst());
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    theme::apply(app, theme::themes().constFirst());
    CHECK(QApplication::palette().color(QPalette::Highlight) == theme::colors().accent);
    testTokens();
    testStyleSheet();
    if (canDrawIcons()) {
        testIcons();
    } else {
        std::printf("FAIL  no SVG image plugin, so no icon draws: install qt6-svg-plugins\n");
        ++g_failures;
    }
    testContrast();
    testSwitch(app);
    if (g_failures == 0)
        std::printf("theme_test: all checks passed\n");
    else
        std::printf("theme_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
