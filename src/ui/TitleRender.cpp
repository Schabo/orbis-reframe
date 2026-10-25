#include "TitleRender.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QTextBoundaryFinder>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

namespace titlerender {

namespace {

// Pictures made lately: a title is re-pushed to playback on every edit, and
// its text only changes on some; a title typing on makes one per letter.
// Kept by number and by size, so a few big export pictures don't pile up.
constexpr size_t kCacheSize = 64;
constexpr size_t kCacheBytes = size_t(256) << 20;
struct Cached {
    title::Style style;
    int fontPx = 0;
    int visible = -1;
    Picture picture;
    size_t bytes() const { return picture.rgba ? picture.rgba->size() : 0; }
};
struct Cache {
    std::mutex mx;
    std::deque<Cached> list; // most recent first
    size_t bytes = 0;
};
Cache &cache()
{
    static Cache c;
    return c;
}

// The text as it is laid out: a line break is a line separator.
QString layoutText(const QString &text)
{
    QString t = text;
    t.replace(QLatin1Char('\n'), QChar::LineSeparator);
    return t;
}

// Where the first `n` graphemes of `text` end.
qsizetype graphemeEnd(const QString &text, int n)
{
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, text);
    for (int i = 0; i < n; ++i)
        if (f.toNextBoundary() < 0)
            return text.size();
    return std::max<qsizetype>(0, f.position());
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

// `visible` < 0: all of the text; else its first `visible` graphemes, the
// picture otherwise as it would be with all of it.
Picture draw(const title::Style &style, int fontPx, int visible)
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
    const QString text = layoutText(style.text);
    // Typed up to here: the text's end, or the end of its first `visible`
    // graphemes.
    const qsizetype shownTo = visible < 0 ? text.size() : graphemeEnd(text, visible);
    QTextLayout layout(text, font);
    layout.setCacheEnabled(true);
    std::vector<double> advance;
    // Typed so far: each line's first `shown` characters, `shownAdvance`
    // wide (all of a line: `advance`).
    std::vector<int> letterCount, shown;
    std::vector<double> shownAdvance;
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
        const int n = static_cast<int>(
            std::clamp<qsizetype>(shownTo - line.textStart(), 0, letters.size()));
        letterCount.push_back(static_cast<int>(letters.size()));
        shown.push_back(n);
        // Measured in the full line's shaping, as its glyphs are drawn.
        shownAdvance.push_back(
            n == letters.size()
                ? advance.back()
                : n == 0 ? 0.0
                         : std::max(0.0, line.cursorToX(line.textStart() + n) -
                                             line.cursorToX(line.textStart()) - letterPx));
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

    // Where each line is drawn: its layout position moved by this. A line
    // being typed lines up by what is typed of it, so its typed part grows
    // from the edge (or the middle) the alignment names.
    auto lineOffset = [&](int i) {
        const double free = textRect.width() - shownAdvance[size_t(i)];
        const double x = style.align == title::Align::Left    ? 0.0
                         : style.align == title::Align::Right ? free
                                                              : free / 2.0;
        return QPointF(textRect.left() + x, textRect.top());
    };
    // A line's glyphs that are typed: those of its first characters, as the
    // whole line shapes them -- no reshaping as letters come.
    const auto whole = [&](int i) { return shown[size_t(i)] == letterCount[size_t(i)]; };
    const auto shownRuns = [&](int i) {
        const QTextLine line = layout.lineAt(i);
        return whole(i)               ? line.glyphRuns()
               : shown[size_t(i)] == 0 ? QList<QGlyphRun>()
                                       : line.glyphRuns(line.textStart(), shown[size_t(i)]);
    };
    QPainterPath letters; // the outline's and the shadow's shapes
    if (outlinePx > 0.0 || style.shadow.on)
        for (int i = 0; i < layout.lineCount(); ++i)
            for (const QGlyphRun &run : shownRuns(i)) {
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
        for (int i = 0; i < layout.lineCount(); ++i) {
            if (whole(i)) {
                layout.lineAt(i).draw(&p, lineOffset(i)); // typed in full
                continue;
            }
            for (const QGlyphRun &run : shownRuns(i))
                p.drawGlyphRun(lineOffset(i), run);
        }
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

int graphemeCount(const QString &text)
{
    const QString t = layoutText(text);
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, t);
    int n = 0;
    while (f.toNextBoundary() >= 0)
        ++n;
    return n;
}

int visibleAt(int count, const inset::Motion &motion, int64_t localMs, int64_t spanMs)
{
    const inset::MotionPhase phase = inset::motionPhase(motion, localMs, spanMs);
    if (!phase.part || phase.part->kind != inset::Motion::Kind::Type)
        return -1;
    // (Less a hair, so a q * count that should be whole isn't rounded up.)
    const int k = static_cast<int>(std::ceil(phase.q * count - 1e-9));
    return k >= count ? -1 : std::max(0, k);
}

Picture cached(const title::Style &style, int fontPx, int visible)
{
    if (visible >= 0 && visible >= graphemeCount(style.text))
        visible = -1; // all of it
    Cache &c = cache();
    std::lock_guard<std::mutex> lk(c.mx);
    for (auto it = c.list.begin(); it != c.list.end(); ++it)
        if (it->fontPx == fontPx && it->visible == visible && it->style == style) {
            Cached hit = *it; // most recent to the front
            c.list.erase(it);
            c.list.push_front(hit);
            return hit.picture;
        }
    return {};
}

Picture render(const title::Style &style, int fontPx, int visible)
{
    if (visible >= 0 && visible >= graphemeCount(style.text))
        visible = -1; // all of it
    if (Picture hit = cached(style, fontPx, visible); !hit.empty())
        return hit;
    // Drawn outside the lock: two threads may both draw the same picture,
    // and the cache then keeps one.
    Picture pic = draw(style, fontPx, visible);
    Cache &c = cache();
    std::lock_guard<std::mutex> lk(c.mx);
    for (const Cached &o : c.list)
        if (o.fontPx == fontPx && o.visible == visible && o.style == style)
            return o.picture;
    c.list.push_front({style, fontPx, visible, pic});
    c.bytes += c.list.front().bytes();
    while (c.list.size() > 1 && (c.list.size() > kCacheSize || c.bytes > kCacheBytes)) {
        c.bytes -= c.list.back().bytes();
        c.list.pop_back();
    }
    return pic;
}

int fontPxFor(float maxSize, int outH, int capPx)
{
    return std::clamp(static_cast<int>(std::lround(double(maxSize) * outH)), 8, std::max(8, capPx));
}

} // namespace titlerender
