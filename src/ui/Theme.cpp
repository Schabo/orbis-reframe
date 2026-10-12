#include "Theme.h"

#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QImageReader>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QRegularExpression>

namespace theme {

namespace {

// "Ember, calm icons": warm orange accent on neutral greys, light-grey icons
// that turn orange under the mouse and when on.
Theme ember()
{
    Theme t;
    t.id = QStringLiteral("ember");
    t.name = QObject::tr("Ember");
    Colors &c = t.colors;
    c.window = QColor(0x1c1d20);
    c.panel = QColor(0x232528);
    c.button = QColor(0x2c2e33);
    c.pressed = QColor(0x25272b);
    c.border = QColor(0x383b41);
    c.borderHover = QColor(0x4a4e56);
    c.menuBar = QColor(0x18191b);
    c.surround = QColor(0x141516);
    c.text = QColor(0xe6e6e6);
    c.textDim = QColor(0xa0a4ab);
    c.textDisabled = QColor(0x565a61);
    c.icon = QColor(0xb9bec6);
    c.iconActive = QColor(0xf5a04a);
    c.accent = QColor(0xf5a04a);
    c.accentSoft = QColor(0x3d2c1c);
    c.textOnAccent = QColor(0x1a1208);
    c.playhead = QColor(0xf5a04a);
    c.clipVideo = QColor(0x3b5873);
    c.clipVideoAlt = QColor(0x34506a);
    c.clipAudio = QColor(0x2f6f6b);
    c.clipOverlay = QColor(0x5e4a80);
    c.clipMuted = QColor(0x44484e);
    c.clipMissing = QColor(0x6b3535);
    c.waveform = QColor(0xcfd6dd);
    c.shade = QColor(0, 0, 0, 77);
    c.error = QColor(0xe05555);
    c.warning = QColor(0xe8c547);
    c.success = QColor(0x5cc96b);
    c.plateDetected = QColor(0x00ff00);
    c.plateTracked = QColor(0x00ffff);
    c.plateFilled = QColor(0xffdc00);
    c.plateUnverified = QColor(0xff00ff);
    c.plateRemoved = QColor(0x8c8c8c);
    c.plateAdded = QColor(0x5999ff);
    return t;
}

int g_current = 0;

using Member = QColor Colors::*;
const QHash<QString, Member> &tokenMembers()
{
    static const QHash<QString, Member> map = {
#define ORBIS_THEME_ENTRY(name, what) {QStringLiteral(#name), &Colors::name},
        ORBIS_THEME_TOKENS(ORBIS_THEME_ENTRY)
#undef ORBIS_THEME_ENTRY
    };
    return map;
}

// Renders a bundled SVG whose strokes and fills say currentColor in the
// colour of the current theme for the mode / state asked for. Pixmaps are
// cached by size and colour, so a theme change just makes new ones.
class TintedSvgEngine : public QIconEngine {
public:
    TintedSvgEngine(const QString &name, IconTone tone) : m_name(name), m_tone(tone)
    {
        QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
        if (f.open(QIODevice::ReadOnly))
            m_svg = f.readAll();
    }

    QIconEngine *clone() const override { return new TintedSvgEngine(m_name, m_tone); }
    QString key() const override { return QStringLiteral("orbis-tinted-svg"); }
    bool isNull() override { return m_svg.isEmpty(); }
    QString iconName() override { return m_name; }

    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }

    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        p->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, dpr));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        const QColor color = colorFor(mode, state);
        const QSize px = size * scale;
        const QString cacheKey = QStringLiteral("%1x%2 %3")
                                     .arg(px.width())
                                     .arg(px.height())
                                     .arg(color.name(QColor::HexArgb));
        if (const auto it = m_cache.constFind(cacheKey); it != m_cache.constEnd())
            return *it;
        QByteArray data = m_svg;
        data.replace("currentColor", color.name().toLatin1());
        QBuffer buffer(&data);
        QImageReader reader(&buffer, "svg");
        reader.setScaledSize(px);
        QPixmap pm = QPixmap::fromImage(reader.read());
        pm.setDevicePixelRatio(scale);
        m_cache.insert(cacheKey, pm);
        return pm;
    }

private:
    QColor colorFor(QIcon::Mode mode, QIcon::State state) const
    {
        const Colors &c = colors();
        if (mode == QIcon::Disabled)
            return c.textDisabled;
        if (mode == QIcon::Active || mode == QIcon::Selected || state == QIcon::On)
            return c.iconActive;
        return m_tone == IconTone::Accent ? c.accent : c.icon;
    }

    QString m_name;
    IconTone m_tone;
    QByteArray m_svg;
    QHash<QString, QPixmap> m_cache;
};

} // namespace

const QList<Theme> &themes()
{
    static const QList<Theme> list = {ember()};
    return list;
}

const Theme &current() { return themes().at(g_current); }

QString cssColor(const QColor &c)
{
    if (c.alpha() == 255)
        return c.name();
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

QString styleSheet(const Colors &colors)
{
    QFile f(QStringLiteral(":/theme.qss"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QString qss = QString::fromUtf8(f.readAll());
    static const QRegularExpression token(QStringLiteral("@([A-Za-z][A-Za-z0-9]*)"));
    QString out;
    out.reserve(qss.size());
    qsizetype last = 0;
    for (auto it = token.globalMatch(qss); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(qss).mid(last, m.capturedStart() - last);
        if (const auto member = tokenMembers().constFind(m.captured(1));
            member != tokenMembers().constEnd())
            out += cssColor(colors.*(*member));
        else
            out += m.captured(0);
        last = m.capturedEnd();
    }
    out += QStringView(qss).mid(last);
    return out;
}

void apply(QApplication &app, const Theme &theme)
{
    for (int i = 0; i < themes().size(); ++i)
        if (themes().at(i).id == theme.id)
            g_current = i;
    const Colors &c = theme.colors;

    // What the stylesheet doesn't reach: rich-text links, painted widgets
    // that ask the palette, and Qt's own fallbacks.
    QPalette pal = QApplication::palette();
    pal.setColor(QPalette::Window, c.window);
    pal.setColor(QPalette::WindowText, c.text);
    pal.setColor(QPalette::Base, c.panel);
    pal.setColor(QPalette::AlternateBase, c.button);
    pal.setColor(QPalette::Text, c.text);
    pal.setColor(QPalette::BrightText, c.text);
    pal.setColor(QPalette::Button, c.button);
    pal.setColor(QPalette::ButtonText, c.text);
    pal.setColor(QPalette::Highlight, c.accent);
    pal.setColor(QPalette::HighlightedText, c.textOnAccent);
    pal.setColor(QPalette::Link, c.accent);
    pal.setColor(QPalette::LinkVisited, c.accent);
    pal.setColor(QPalette::PlaceholderText, c.textDim);
    pal.setColor(QPalette::ToolTipBase, c.button);
    pal.setColor(QPalette::ToolTipText, c.text);
    pal.setColor(QPalette::Light, c.borderHover);
    pal.setColor(QPalette::Midlight, c.border);
    pal.setColor(QPalette::Mid, c.border);
    pal.setColor(QPalette::Dark, c.menuBar);
    pal.setColor(QPalette::Shadow, c.surround);
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, c.textDisabled);
    QApplication::setPalette(pal);
    app.setStyleSheet(styleSheet(c));
}

QIcon icon(const QString &name, IconTone tone) { return QIcon(new TintedSvgEngine(name, tone)); }

} // namespace theme
