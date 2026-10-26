#include "GaugeRender.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <numbers>

namespace gaugerender {

namespace {

// Pictures made lately: a gauge makes one per step its needle or tape
// moves, and a ride sweeps back and forth over the same ones. Kept by
// number and by size, so a few big export pictures don't pile up.
constexpr size_t kCacheSize = 512;
constexpr size_t kCacheBytes = size_t(256) << 20;
struct Cached {
    gauge::Style style;
    int heightPx = 0;
    Reading reading;
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

// A tape shows this much of the compass, either side of its marker.
constexpr double kTapeHalfDeg = 60.0;

// How far the needle's tip, the bar's end or the tape moves per unit, in
// pixels of a picture `h` high: what the reading's position is rounded to.
double pixelsPerUnit(const gauge::Style &s, double h)
{
    switch (s.kind) {
    case gauge::Kind::Dial: // the needle, 0.8 of the radius, over 270 degrees
        return 1.5 * std::numbers::pi * 0.8 * 0.48 * h / (s.max - s.min);
    case gauge::Kind::Bar: return (gauge::aspect(s.kind) - 0.16) * h / (s.max - s.min);
    case gauge::Kind::Tape: return (gauge::aspect(s.kind) * h) / (2.0 * kTapeHalfDeg);
    }
    return 1.0;
}

// A scale's step between labelled ticks: 1, 2, 2.5 or 5 times a power of
// ten, about `count` of them over `span`.
double niceStep(double span, int count)
{
    const double raw = span / count;
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0})
        if (raw <= m * p)
            return m * p;
    return 10.0 * p;
}

// The unlabelled ticks between: quarters of a 2, fifths of the rest.
double minorStep(double step)
{
    const double lead = step / std::pow(10.0, std::floor(std::log10(step)));
    return step / (std::abs(lead - 2.0) < 1e-9 ? 4.0 : 5.0);
}

QColor rgbColor(uint32_t rgb, double alpha = 1.0)
{
    QColor c = QColor::fromRgb(rgb & 0xffffff);
    c.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return c;
}

QFont fontAt(const gauge::Style &s, double px, bool bold)
{
    QFont f(s.font);
    f.setPixelSize(std::max(1, int(std::lround(px))));
    f.setBold(bold);
    // Every digit as wide: the number doesn't shift as it changes.
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
}

// `text` centred on `x`, its baseline at `y`.
void textAt(QPainter &p, const QString &text, double x, double y)
{
    const double w = QFontMetricsF(p.font()).horizontalAdvance(text);
    p.drawText(QPointF(x - w / 2.0, y), text);
}

QString numberText(const Reading &r)
{
    return r.hasValue ? QLocale().toString(r.number) : QStringLiteral("--");
}

QString unitText(const gauge::Style &s)
{
    if (s.kind == gauge::Kind::Tape)
        return {};
    return QLatin1String(s.reads == gauge::Reads::Speed ? title::unitName(s.units.speed)
                                                        : title::unitName(s.units.altitude));
}

QString scaleLabel(double v, double step)
{
    const int decimals = step >= 1.0 ? 0 : step >= 0.1 ? 1 : 2;
    return QLocale().toString(v, 'f', decimals);
}

void drawDial(QPainter &p, const gauge::Style &s, const Reading &r, double h)
{
    const QPointF c(h / 2.0, h / 2.0);
    const double R = 0.48 * h;
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.face, s.faceOpacity));
    p.drawEllipse(c, R, R);

    // 270 degrees, from the lower left round over the top to the lower right.
    const auto angleOf = [&](double v) {
        const double f = std::clamp((v - s.min) / (s.max - s.min), 0.0, 1.0);
        return (225.0 - 270.0 * f) * std::numbers::pi / 180.0;
    };
    const auto at = [&](double a, double radius) {
        return QPointF(c.x() + radius * std::cos(a), c.y() - radius * std::sin(a));
    };
    const double step = niceStep(s.max - s.min, 6);
    const double minor = minorStep(step);
    p.setFont(fontAt(s, 0.085 * h, false));
    const QFontMetricsF fm(p.font());
    for (double v = std::ceil(s.min / minor - 1e-9) * minor; v <= s.max + 1e-9; v += minor) {
        const bool major = std::abs(std::remainder(v, step)) < minor * 1e-3;
        const double a = angleOf(v);
        p.setPen(QPen(rgbColor(s.text, major ? 1.0 : 0.7), (major ? 0.012 : 0.006) * h,
                      Qt::SolidLine, Qt::FlatCap));
        p.drawLine(at(a, (major ? 0.84 : 0.91) * R), at(a, 0.97 * R));
        if (major) {
            p.setPen(rgbColor(s.text));
            const QPointF l = at(a, 0.68 * R);
            textAt(p, scaleLabel(v, step), l.x(), l.y() + fm.capHeight() / 2.0);
        }
    }
    if (r.hasValue) {
        // The scale lit up to the value along its rim, and the needle.
        const double from = angleOf(s.min), to = angleOf(r.position);
        QPainterPath arc;
        const QRectF rim(c.x() - 0.97 * R, c.y() - 0.97 * R, 1.94 * R, 1.94 * R);
        arc.arcMoveTo(rim, from * 180.0 / std::numbers::pi);
        arc.arcTo(rim, from * 180.0 / std::numbers::pi, (to - from) * 180.0 / std::numbers::pi);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(rgbColor(s.accent), 0.035 * h, Qt::SolidLine, Qt::FlatCap));
        p.drawPath(arc);
        p.setPen(QPen(rgbColor(s.accent), 0.025 * h, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(at(to + std::numbers::pi, 0.12 * R), at(to, 0.80 * R));
        p.setPen(Qt::NoPen);
        p.setBrush(rgbColor(s.accent));
        p.drawEllipse(c, 0.06 * h, 0.06 * h);
    }
    p.setPen(rgbColor(s.text));
    p.setFont(fontAt(s, 0.16 * h, true));
    textAt(p, numberText(r), c.x(), c.y() + 0.52 * R);
    p.setFont(fontAt(s, 0.07 * h, false));
    textAt(p, unitText(s), c.x(), c.y() + 0.70 * R);
}

void drawBar(QPainter &p, const gauge::Style &s, const Reading &r, double w, double h)
{
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.face, s.faceOpacity));
    p.drawRoundedRect(QRectF(0, 0, w, h), 0.15 * h, 0.15 * h);
    const double pad = 0.08 * h;
    // The number and its unit, over the bar.
    p.setPen(rgbColor(s.text));
    const QFont big = fontAt(s, 0.42 * h, true);
    p.setFont(big);
    const QString number = numberText(r);
    const double baseline = 0.50 * h;
    p.drawText(QPointF(pad + 0.04 * h, baseline), number);
    const double numberW = QFontMetricsF(big).horizontalAdvance(number);
    p.setFont(fontAt(s, 0.18 * h, false));
    p.drawText(QPointF(pad + 0.04 * h + numberW + 0.06 * h, baseline), unitText(s));
    // The bar along its scale, its ends labelled.
    const QRectF track(pad, 0.60 * h, w - 2.0 * pad, 0.13 * h);
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.text, 0.25));
    p.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);
    if (r.hasValue) {
        const double f = std::clamp((r.position - s.min) / (s.max - s.min), 0.0, 1.0);
        if (f > 0.0) {
            QRectF fill = track;
            fill.setWidth(std::max(track.height(), track.width() * f));
            p.setBrush(rgbColor(s.accent));
            p.drawRoundedRect(fill, track.height() / 2.0, track.height() / 2.0);
        }
    }
    const double step = niceStep(s.max - s.min, 4);
    p.setPen(rgbColor(s.text, 0.8));
    p.setFont(fontAt(s, 0.12 * h, false));
    const QFontMetricsF fm(p.font());
    const QString lo = scaleLabel(s.min, step), hi = scaleLabel(s.max, step);
    p.drawText(QPointF(pad, 0.90 * h), lo);
    p.drawText(QPointF(w - pad - fm.horizontalAdvance(hi), 0.90 * h), hi);
}

void drawTape(QPainter &p, const gauge::Style &s, const Reading &r, double w, double h)
{
    QPainterPath face;
    face.addRoundedRect(QRectF(0, 0, w, h), 0.15 * h, 0.15 * h);
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.face, s.faceOpacity));
    p.drawPath(face);
    // The heading as a number at the top, the marker under it.
    p.setPen(rgbColor(s.text));
    p.setFont(fontAt(s, 0.26 * h, true));
    textAt(p, r.hasValue ? numberText(r) + QChar(0x00b0) : QStringLiteral("--"), w / 2.0,
           0.32 * h);
    const double top = 0.42 * h;
    if (!r.hasValue)
        return;
    QPainterPath marker;
    marker.moveTo(w / 2.0 - 0.07 * h, top - 0.02 * h);
    marker.lineTo(w / 2.0 + 0.07 * h, top - 0.02 * h);
    marker.lineTo(w / 2.0, top + 0.08 * h);
    marker.closeSubpath();
    p.setBrush(rgbColor(s.accent));
    p.drawPath(marker);
    // The compass, sliding: ticks every 5 degrees, longer every 15, the
    // points of the compass every 45; it fades out towards the ends.
    p.save();
    p.setClipPath(face);
    const double perDeg = w / (2.0 * kTapeHalfDeg);
    static const char *const points[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const QFont cardinal = fontAt(s, 0.22 * h, true), between = fontAt(s, 0.16 * h, true),
                degrees = fontAt(s, 0.12 * h, false);
    const int first = int(std::ceil((r.position - kTapeHalfDeg) / 5.0)) * 5;
    for (int d = first; d <= r.position + kTapeHalfDeg; d += 5) {
        const double x = w / 2.0 + (d - r.position) * perDeg;
        const double fade = 1.0 - std::pow(std::abs(d - r.position) / kTapeHalfDeg, 2.0);
        const int deg = ((d % 360) + 360) % 360;
        const bool quarter = deg % 45 == 0, fifteen = deg % 15 == 0;
        p.setPen(QPen(rgbColor(s.text, fade), (fifteen ? 0.012 : 0.007) * h));
        p.drawLine(QPointF(x, h - 0.06 * h), QPointF(x, h - (fifteen ? 0.17 : 0.11) * h));
        p.setPen(rgbColor(s.text, fade));
        if (quarter) {
            p.setFont(deg % 90 == 0 ? cardinal : between);
            textAt(p, QLatin1String(points[deg / 45]), x, h - 0.24 * h);
        } else if (fifteen) {
            p.setFont(degrees);
            textAt(p, QString::number(deg), x, h - 0.24 * h);
        }
    }
    p.restore();
}

Picture draw(const gauge::Style &s, int heightPx, const Reading &r)
{
    const int h = std::max(8, heightPx);
    const int w = std::max(8, int(std::lround(gauge::aspect(s.kind) * h)));
    QImage img(w, h, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        switch (s.kind) {
        case gauge::Kind::Dial: drawDial(p, s, r, h); break;
        case gauge::Kind::Bar: drawBar(p, s, r, w, h); break;
        case gauge::Kind::Tape: drawTape(p, s, r, w, h); break;
        }
    }
    auto rgba = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(rgba->data() + size_t(y) * w * 4, img.constScanLine(y), size_t(w) * 4);
    Picture pic;
    pic.rgba = std::move(rgba);
    pic.width = w;
    pic.height = h;
    return pic;
}

} // namespace

Reading readingOf(const gauge::Style &s, const std::optional<Telemetry::Values> &gps, int heightPx)
{
    Reading r;
    if (!gps)
        return r;
    double v = 0.0;
    if (s.kind == gauge::Kind::Tape) {
        if (!gps->headingDeg)
            return r;
        v = *gps->headingDeg;
        r.number = int(std::lround(v)) % 360;
    } else {
        v = s.reads == gauge::Reads::Speed ? title::speedIn(gps->speedMps, s.units.speed)
                                           : title::altitudeIn(gps->altitudeM, s.units.altitude);
        r.number = int(std::lround(v));
        v = std::clamp(v, s.min, s.max);
    }
    r.hasValue = true;
    // Half a pixel's move: finer doesn't show.
    const double quantum = 0.5 / std::max(1e-9, pixelsPerUnit(s, std::max(8, heightPx)));
    r.position = std::round(v / quantum) * quantum;
    return r;
}

Picture cached(const gauge::Style &style, int heightPx, const Reading &reading)
{
    Cache &c = cache();
    std::lock_guard<std::mutex> lk(c.mx);
    for (auto it = c.list.begin(); it != c.list.end(); ++it)
        if (it->heightPx == heightPx && it->reading == reading && it->style == style) {
            Cached hit = *it; // most recent to the front
            c.list.erase(it);
            c.list.push_front(hit);
            return hit.picture;
        }
    return {};
}

Picture render(const gauge::Style &style, int heightPx, const Reading &reading)
{
    if (Picture hit = cached(style, heightPx, reading); !hit.empty())
        return hit;
    // Drawn outside the lock: two threads may both draw the same picture,
    // and the cache then keeps one.
    Picture pic = draw(style, heightPx, reading);
    Cache &c = cache();
    std::lock_guard<std::mutex> lk(c.mx);
    for (const Cached &o : c.list)
        if (o.heightPx == heightPx && o.reading == reading && o.style == style)
            return o.picture;
    c.list.push_front({style, heightPx, reading, pic});
    c.bytes += c.list.front().bytes();
    while (c.list.size() > 1 && (c.list.size() > kCacheSize || c.bytes > kCacheBytes)) {
        c.bytes -= c.list.back().bytes();
        c.list.pop_back();
    }
    return pic;
}

} // namespace gaugerender
