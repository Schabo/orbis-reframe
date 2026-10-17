#include "TitleRender.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace titlerender {

namespace {

// Pictures made lately: a title is re-pushed to playback on every edit, and
// its text only changes on some.
constexpr size_t kCacheSize = 32;
struct Cached {
    title::Style style;
    int fontPx = 0;
    Picture picture;
};
std::deque<Cached> &cache()
{
    static std::deque<Cached> c;
    return c;
}

Picture draw(const title::Style &style, int fontPx)
{
    Picture pic;
    if (style.text.trimmed().isEmpty() || fontPx <= 0)
        return pic;
    QFont font(style.font);
    font.setPixelSize(fontPx);
    font.setBold(style.bold);
    font.setItalic(style.italic);
    // Unhinted: the shapes then scale evenly, so the view (rendered smaller)
    // and the export match.
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(font);
    const QStringList lines = style.text.split(QLatin1Char('\n'));
    double textW = 0.0;
    for (const QString &l : lines)
        textW = std::max(textW, fm.horizontalAdvance(l));
    const double textH = fm.lineSpacing() * (lines.size() - 1) + fm.height();
    // Room for an italic's overhang and the antialiased edge.
    const int padX = static_cast<int>(std::ceil(fontPx * 0.25));
    const int padY = static_cast<int>(std::ceil(fontPx * 0.1));
    const int w = static_cast<int>(std::ceil(textW)) + 2 * padX;
    const int h = static_cast<int>(std::ceil(textH)) + 2 * padY;

    QImage img(w, h, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        p.setFont(font);
        p.setPen(QColor::fromRgb(style.rgb & 0xffffff));
        p.drawText(QRectF(padX, padY, std::ceil(textW), std::ceil(textH)),
                   Qt::AlignHCenter | Qt::AlignTop, style.text);
    }
    auto rgba = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(rgba->data() + size_t(y) * w * 4, img.constScanLine(y), size_t(w) * 4);
    pic.rgba = std::move(rgba);
    pic.width = w;
    pic.height = h;
    pic.widthPerFontPx = double(w) / fontPx;
    return pic;
}

} // namespace

Picture render(const title::Style &style, int fontPx)
{
    std::deque<Cached> &c = cache();
    for (auto it = c.begin(); it != c.end(); ++it)
        if (it->fontPx == fontPx && it->style == style) {
            Cached hit = *it; // most recent to the front
            c.erase(it);
            c.push_front(hit);
            return hit.picture;
        }
    Picture pic = draw(style, fontPx);
    c.push_front({style, fontPx, pic});
    if (c.size() > kCacheSize)
        c.pop_back();
    return pic;
}

int fontPxFor(float maxSize, int outH, int capPx)
{
    return std::clamp(static_cast<int>(std::lround(double(maxSize) * outH)), 8, std::max(8, capPx));
}

} // namespace titlerender
