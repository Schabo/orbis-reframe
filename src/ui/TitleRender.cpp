#include "TitleRender.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QTextLayout>

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

QColor colorOf(uint32_t rgb, float opacity = 1.0f)
{
    QColor c = QColor::fromRgb(rgb & 0xffffff);
    c.setAlphaF(std::clamp(opacity, 0.0f, 1.0f));
    return c;
}

// One pass of a box blur of radius `r` along rows (`horizontal`) or columns,
// outside the image counting as clear.
void boxBlurPass(QImage &a, int r, bool horizontal)
{
    const int w = a.width(), h = a.height();
    const int n = horizontal ? w : h;
    const int lines = horizontal ? h : w;
    std::vector<int> src(static_cast<size_t>(n));
    for (int l = 0; l < lines; ++l) {
        auto px = [&](int i) -> uchar & {
            return horizontal ? a.scanLine(l)[i] : a.scanLine(i)[l];
        };
        for (int i = 0; i < n; ++i)
            src[size_t(i)] = px(i);
        int sum = 0;
        for (int i = 0; i < std::min(r, n); ++i)
            sum += src[size_t(i)];
        const int span = 2 * r + 1;
        for (int i = 0; i < n; ++i) {
            if (i + r < n)
                sum += src[size_t(i + r)];
            if (i - r - 1 >= 0)
                sum -= src[size_t(i - r - 1)];
            px(i) = uchar((sum + span / 2) / span);
        }
    }
}

// Three box passes each way: close to a Gaussian, reaching `extentPx` out.
void blur(QImage &alpha, double extentPx)
{
    const int r = static_cast<int>(extentPx / 3.0);
    if (r < 1)
        return;
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurPass(alpha, r, true);
        boxBlurPass(alpha, r, false);
    }
}

Picture draw(const title::Style &style, int fontPx)
{
    Picture pic;
    if (style.text.trimmed().isEmpty() || fontPx <= 0)
        return pic;
    const double em = fontPx;
    QFont font(style.font);
    font.setPixelSize(fontPx);
    font.setBold(style.bold);
    font.setItalic(style.italic);
    // Unhinted: the shapes then scale evenly, so the view (rendered smaller)
    // and the export match.
    font.setHintingPreference(QFont::PreferNoHinting);
    const double letterPx = style.letterSpacing * em;
    if (letterPx != 0.0)
        font.setLetterSpacing(QFont::AbsoluteSpacing, letterPx);
    const QFontMetricsF fm(font);

    // Laid out as QPainter lays out text in a rectangle: lines on whole
    // pixels, each a QTextLine's height (times the line spacing) below the
    // last, each lined up by its advance -- less the spacing after its last
    // letter, so the letters themselves line up. Not clipped to that
    // rectangle, as QPainter would: an italic's overhang and the last line's
    // descenders reach past it.
    QString text = style.text;
    text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout layout(text, font);
    layout.setCacheEnabled(true);
    std::vector<double> advance;
    double textW = 0.0;
    double textH = 0.0;
    layout.beginLayout();
    for (double y = -fm.leading();;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(0x01000000);
        y = std::ceil(y + fm.leading());
        line.setPosition(QPointF(0.0, y));
        textH = y + line.height();
        y += line.height() * style.lineSpacing;
        // A line's text ends with the separator to the next.
        const QString letters =
            text.mid(line.textStart(), line.textLength()).remove(QChar::LineSeparator);
        const double trailing = letters.isEmpty() ? 0.0 : letterPx;
        advance.push_back(std::max(0.0, line.horizontalAdvance() - trailing));
        textW = std::max(textW, advance.back());
    }
    layout.endLayout();

    // What reaches past the letters: their outline, and the shadow beyond it.
    const double outlinePx = style.outline.on ? style.outline.width * em : 0.0;
    const double shadowDx = style.shadow.on ? style.shadow.distance * em * M_SQRT1_2 : 0.0;
    const double shadowBlurPx = style.shadow.on ? style.shadow.blur * em : 0.0;
    const double beyondLetters =
        style.shadow.on ? outlinePx + shadowDx + shadowBlurPx : outlinePx;
    const double boxPadPx = style.box.on ? style.box.padding * em : 0.0;
    // Room for an italic's overhang and the antialiased edge, and for what
    // is drawn round the text. The same on both sides, so the text's middle
    // is the picture's.
    const auto pad = [&](double letterRoom) {
        return static_cast<int>(std::max(std::ceil(letterRoom + beyondLetters),
                                         boxPadPx > 0.0 ? std::ceil(boxPadPx) + 1.0 : 0.0));
    };
    const int padX = pad(fontPx * 0.25);
    const int padY = pad(fontPx * 0.1);
    const QRectF textRect(padX, padY, std::ceil(textW), std::ceil(textH));
    const int w = static_cast<int>(textRect.width()) + 2 * padX;
    const int h = static_cast<int>(textRect.height()) + 2 * padY;

    // Where each line is drawn: its layout position moved by this.
    auto lineOffset = [&](int i) {
        const double free = textRect.width() - advance[size_t(i)];
        const double x = style.align == title::Align::Left    ? 0.0
                         : style.align == title::Align::Right ? free
                                                              : free / 2.0;
        return QPointF(textRect.left() + x, textRect.top());
    };
    QPainterPath letters; // the outline's and the shadow's shapes
    if (outlinePx > 0.0 || style.shadow.on)
        for (int i = 0; i < layout.lineCount(); ++i)
            for (const QGlyphRun &run : layout.lineAt(i).glyphRuns()) {
                const QRawFont raw = run.rawFont();
                const QList<quint32> glyphs = run.glyphIndexes();
                const QList<QPointF> at = run.positions();
                for (qsizetype g = 0; g < glyphs.size(); ++g)
                    letters.addPath(raw.pathForGlyph(glyphs[g]).translated(at[g] + lineOffset(i)));
            }
    letters.setFillRule(Qt::WindingFill);
    const QPen outlinePen(Qt::black, 2.0 * outlinePx, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

    QImage img(w, h, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        if (style.box.on) {
            const QRectF box = textRect.adjusted(-boxPadPx, -boxPadPx, boxPadPx, boxPadPx);
            const double radius = style.box.cornerRadius * std::min(box.width(), box.height());
            p.setPen(Qt::NoPen);
            p.setBrush(colorOf(style.box.rgb, style.box.opacity));
            p.drawRoundedRect(box, radius, radius);
        }
        if (style.shadow.on && style.shadow.opacity > 0.0f) {
            QImage alpha(w, h, QImage::Format_Alpha8);
            alpha.fill(0);
            {
                QPainter sp(&alpha);
                sp.setRenderHint(QPainter::Antialiasing);
                sp.translate(shadowDx, shadowDx);
                if (outlinePx > 0.0)
                    sp.strokePath(letters, outlinePen);
                sp.fillPath(letters, Qt::black);
            }
            blur(alpha, shadowBlurPx);
            QImage shadow(w, h, QImage::Format_RGBA8888_Premultiplied);
            const QColor c = colorOf(style.shadow.rgb);
            const double gain = std::clamp(style.shadow.opacity, 0.0f, 1.0f) / 255.0;
            for (int y = 0; y < h; ++y) {
                const uchar *a = alpha.constScanLine(y);
                uchar *o = shadow.scanLine(y);
                for (int x = 0; x < w; ++x) {
                    const double k = a[x] * gain; // premultiplied
                    o[4 * x + 0] = uchar(std::lround(c.red() * k));
                    o[4 * x + 1] = uchar(std::lround(c.green() * k));
                    o[4 * x + 2] = uchar(std::lround(c.blue() * k));
                    o[4 * x + 3] = uchar(std::lround(255.0 * k));
                }
            }
            p.drawImage(0, 0, shadow);
        }
        if (outlinePx > 0.0) {
            QPen pen = outlinePen;
            pen.setColor(colorOf(style.outline.rgb));
            p.strokePath(letters, pen);
        }
        p.setPen(colorOf(style.rgb));
        for (int i = 0; i < layout.lineCount(); ++i)
            layout.lineAt(i).draw(&p, lineOffset(i));
    }
    auto rgba = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(rgba->data() + size_t(y) * w * 4, img.constScanLine(y), size_t(w) * 4);
    pic.rgba = std::move(rgba);
    pic.width = w;
    pic.height = h;
    pic.widthPerFontPx = double(w) / fontPx;
    // The point the look's centreX names: the text's (or its box's) edge.
    switch (style.align) {
    case title::Align::Left: pic.anchorX = (textRect.left() - boxPadPx) / w; break;
    case title::Align::Right: pic.anchorX = (textRect.right() + boxPadPx) / w; break;
    case title::Align::Centre: pic.anchorX = 0.5; break;
    }
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
