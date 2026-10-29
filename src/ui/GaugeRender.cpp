#include "GaugeRender.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
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
    uint64_t routeKey = 0;
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

// A lean dial's half circle: its centre this far down the picture, its
// radius in picture heights.
constexpr double kLeanCentreY = 0.92;
constexpr double kLeanRadius = 0.86;
// A G circle's face radius in picture heights, and how far out the scale's
// end is, in face radii.
constexpr double kGFaceRadius = 0.48;
constexpr double kGPlot = 0.82;
// A whole trip's margin round its square, in picture heights.
constexpr double kRouteMargin = 0.1;
// A route ahead: its round face's radius in picture heights; the dot this
// far below the centre and the road shown this far out, in face radii.
constexpr double kAheadFace = 0.48;
constexpr double kAheadDot = 0.4;
constexpr double kAheadReach = 0.9;
// ... reaching at least this far from the dot, in metres, so a stop doesn't
// zoom in to the street.
constexpr double kAheadMinM = 150.0;

// How far the needle's tip, the bar's end or the tape moves per unit, in
// pixels of a picture `h` high: what the reading's position is rounded to.
double pixelsPerUnit(const gauge::Style &s, double h)
{
    switch (s.kind) {
    case gauge::Kind::Dial: // the needle, 0.8 of the radius, over 270 degrees
        return 1.5 * std::numbers::pi * 0.8 * 0.48 * h / (s.max - s.min);
    case gauge::Kind::Bar: return (gauge::aspect(s.kind) - 0.16) * h / (s.max - s.min);
    case gauge::Kind::Tape: return (gauge::aspect(s.kind) * h) / (2.0 * kTapeHalfDeg);
    case gauge::Kind::Lean: // the pointer's tip, at the rim, over 180 degrees
        return std::numbers::pi * kLeanRadius * h / (2.0 * s.max);
    case gauge::Kind::GForce: return kGPlot * kGFaceRadius * h / s.max;
    case gauge::Kind::Route: return 1.0; // by the route's scale: readingOf
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

void drawLean(QPainter &p, const gauge::Style &s, const Reading &r, double w, double h)
{
    const QPointF c(w / 2.0, kLeanCentreY * h);
    const double R = kLeanRadius * h;
    // The face: the upper half of a disc on a narrow base.
    QPainterPath face;
    face.moveTo(c.x() - R, c.y());
    face.arcTo(QRectF(c.x() - R, c.y() - R, 2.0 * R, 2.0 * R), 180.0, -180.0);
    face.lineTo(c.x() + R, h);
    face.lineTo(c.x() - R, h);
    face.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.face, s.faceOpacity));
    p.drawPath(face);

    // Upright at the top, the scale's ends level either side: leaning
    // right turns it right.
    const auto angleOf = [&](double v) {
        const double f = std::clamp(v / s.max, -1.0, 1.0);
        return (90.0 - 90.0 * f) * std::numbers::pi / 180.0;
    };
    const auto at = [&](double a, double radius) {
        return QPointF(c.x() + radius * std::cos(a), c.y() - radius * std::sin(a));
    };
    const double step = niceStep(s.max, 3);
    const double minor = minorStep(step);
    p.setFont(fontAt(s, 0.085 * h, false));
    const QFontMetricsF fm(p.font());
    for (double v = -std::floor(s.max / minor + 1e-9) * minor; v <= s.max + 1e-9; v += minor) {
        const bool major = std::abs(std::remainder(v, step)) < minor * 1e-3;
        const double a = angleOf(v);
        p.setPen(QPen(rgbColor(s.text, major ? 1.0 : 0.7), (major ? 0.012 : 0.006) * h,
                      Qt::SolidLine, Qt::FlatCap));
        p.drawLine(at(a, (major ? 0.84 : 0.91) * R), at(a, 0.97 * R));
        // The labels leave the ends to the base line.
        if (major && std::abs(v) < s.max - minor * 1e-3) {
            p.setPen(rgbColor(s.text));
            const QPointF l = at(a, 0.72 * R);
            textAt(p, scaleLabel(std::abs(v), step), l.x(), l.y() + fm.capHeight() / 2.0);
        }
    }
    if (r.hasValue) {
        // The rim lit from upright to the lean, and a pointer there.
        const double from = angleOf(0.0), to = angleOf(r.position);
        QPainterPath arc;
        const QRectF rim(c.x() - 0.97 * R, c.y() - 0.97 * R, 1.94 * R, 1.94 * R);
        arc.arcMoveTo(rim, from * 180.0 / std::numbers::pi);
        arc.arcTo(rim, from * 180.0 / std::numbers::pi, (to - from) * 180.0 / std::numbers::pi);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(rgbColor(s.accent), 0.04 * h, Qt::SolidLine, Qt::FlatCap));
        p.drawPath(arc);
        // Over the ticks, clear of the labels inside them.
        p.setPen(QPen(rgbColor(s.accent), 0.035 * h, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(at(to, 0.80 * R), at(to, 1.0 * R));
    }
    // The degrees in the middle, the side under them.
    p.setPen(rgbColor(s.text));
    p.setFont(fontAt(s, 0.26 * h, true));
    const int degrees = std::abs(r.number);
    textAt(p,
           (r.hasValue ? QLocale().toString(degrees) : QStringLiteral("--")) + QChar(0x00b0),
           c.x(), c.y() - 0.20 * R);
    if (r.hasValue) {
        p.setFont(fontAt(s, 0.11 * h, true));
        textAt(p, title::leanSide(r.number), c.x(), c.y() - 0.02 * R);
    }
}

void drawGForce(QPainter &p, const gauge::Style &s, const Reading &r, double h)
{
    const QPointF c(h / 2.0, h / 2.0);
    const double R = kGFaceRadius * h;
    const double plot = kGPlot * R; // the scale's end
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.face, s.faceOpacity));
    p.drawEllipse(c, R, R);
    // Rings at even steps of g, the cross of the two axes.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(rgbColor(s.text, 0.45), 0.006 * h));
    p.drawLine(QPointF(c.x() - plot, c.y()), QPointF(c.x() + plot, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - plot), QPointF(c.x(), c.y() + plot));
    const double step = niceStep(s.max, 2);
    p.setFont(fontAt(s, 0.07 * h, false));
    for (double v = step; v <= s.max + 1e-9; v += step) {
        const double rr = plot * v / s.max;
        const bool outer = v > s.max - step + 1e-9;
        p.setPen(QPen(rgbColor(s.text, outer ? 0.9 : 0.5), (outer ? 0.012 : 0.007) * h));
        p.drawEllipse(c, rr, rr);
        // Labelled up and to the right, off the axes.
        const double a = std::numbers::pi / 4.0;
        p.setPen(rgbColor(s.text, 0.8));
        p.drawText(
            QPointF(c.x() + rr * std::cos(a) + 0.01 * h, c.y() - rr * std::sin(a) - 0.01 * h),
            scaleLabel(v, step));
    }
    // The total in the lower half, between the rings; the dot over it.
    p.setPen(rgbColor(s.text));
    const QFont big = fontAt(s, 0.13 * h, true), small = fontAt(s, 0.08 * h, false);
    const QString number = r.hasValue ? title::gNumber(r.number, QLocale()) : QStringLiteral("--");
    const QString unit = QStringLiteral(" g");
    const double numberW = QFontMetricsF(big).horizontalAdvance(number);
    const double left = c.x() - (numberW + QFontMetricsF(small).horizontalAdvance(unit)) / 2.0;
    const double baseline = c.y() + 0.66 * R;
    p.setFont(big);
    p.drawText(QPointF(left, baseline), number);
    p.setFont(small);
    p.drawText(QPointF(left + numberW, baseline), unit);
    if (!r.hasValue)
        return;
    // Across the travel to the right, speeding up upwards; past the scale
    // the dot stays on its edge, the way it points.
    double x = r.position / s.max, y = r.position2 / s.max;
    const double len = std::hypot(x, y);
    if (len > 1.0) {
        x /= len;
        y /= len;
    }
    const QPointF dot(c.x() + x * plot, c.y() - y * plot);
    p.setPen(QPen(rgbColor(s.text), 0.012 * h));
    p.setBrush(rgbColor(s.accent));
    p.drawEllipse(dot, 0.055 * h, 0.055 * h);
}

// Calls `segment(a, b)` for each piece of `route`'s line from `from` to `to`
// along it, the pieces at either end cut there; in metres.
template <typename F>
void walkRoute(const Route &route, double from, double to, F segment)
{
    for (const std::vector<Route::Point> &st : route.stretches)
        for (size_t i = 1; i < st.size(); ++i) {
            const Route::Point &a = st[i - 1], &b = st[i];
            if (a.along >= to)
                return;
            if (b.along <= from)
                continue;
            const auto at = [&](double along) {
                const double f = b.along > a.along ? (along - a.along) / (b.along - a.along) : 0.0;
                return QPointF(a.x + f * (b.x - a.x), a.y + f * (b.y - a.y));
            };
            segment(a.along >= from ? QPointF(a.x, a.y) : at(from),
                    b.along <= to ? QPointF(b.x, b.y) : at(to));
        }
}

// The point `along` the route: in a gap, where the stretch before ended.
QPointF routePoint(const Route &route, double along)
{
    QPointF at(route.stretches.front().front().x, route.stretches.front().front().y);
    walkRoute(route, 0.0, along, [&](QPointF, QPointF b) { at = b; });
    return at;
}

// The route's line from `from` to `to` along it, each point placed by `px`.
template <typename Px>
void drawRouteLine(QPainter &p, const Route &route, double from, double to, Px px,
                   const QPen &pen)
{
    QPainterPath path;
    QPointF last(std::nan(""), 0.0);
    walkRoute(route, from, to, [&](QPointF a, QPointF b) {
        if (a != last)
            path.moveTo(px(a));
        path.lineTo(px(b));
        last = b;
    });
    p.setBrush(Qt::NoBrush);
    p.setPen(pen);
    p.drawPath(path);
}

// The whole trip in a square, north up, its shape kept.
void drawTrip(QPainter &p, const gauge::Style &s, const Reading &r, const Route &route, double h)
{
    QPainterPath face;
    face.addRoundedRect(QRectF(0, 0, h, h), 0.12 * h, 0.12 * h);
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.face, s.faceOpacity));
    p.drawPath(face);
    const double m = kRouteMargin * h, side = h - 2.0 * m;
    // A trip that hardly moves stays a small mark in the middle: at least
    // 100 m across.
    const double extent = std::max({route.xHi - route.xLo, route.yHi - route.yLo, 100.0});
    const double cx = (route.xLo + route.xHi) / 2.0, cy = (route.yLo + route.yHi) / 2.0;
    const auto px = [&](QPointF q) {
        return QPointF(h / 2.0 + (q.x() - cx) / extent * side,
                       h / 2.0 - (q.y() - cy) / extent * side);
    };
    // North, in the corner.
    p.setPen(rgbColor(s.text, 0.8));
    p.setFont(fontAt(s, 0.075 * h, true));
    textAt(p, QStringLiteral("N"), h - 0.09 * h, 0.12 * h);
    // The whole route faint, the part ridden in the accent over it.
    drawRouteLine(p, route, 0.0, std::numeric_limits<double>::infinity(), px,
                  QPen(rgbColor(s.text, 0.4), 0.02 * h, Qt::SolidLine, Qt::RoundCap,
                       Qt::RoundJoin));
    drawRouteLine(p, route, 0.0, r.position, px,
                  QPen(rgbColor(s.accent), 0.03 * h, Qt::SolidLine, Qt::RoundCap,
                       Qt::RoundJoin));
    // A stretch that stays in one place (the GPS repeating a fix it lost)
    // has no line to draw: a mark where it is.
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.text, 0.4));
    for (const std::vector<Route::Point> &st : route.stretches)
        if (st.back().along - st.front().along < 1e-9)
            p.drawEllipse(px(QPointF(st.front().x, st.front().y)), 0.02 * h, 0.02 * h);
    if (!r.hasValue)
        return;
    p.setPen(QPen(rgbColor(s.text), 0.012 * h));
    p.setBrush(rgbColor(s.accent));
    p.drawEllipse(px(routePoint(route, r.position)), 0.045 * h, 0.045 * h);
}

// The road around the camera on a round face, turning with it: the way it
// travels up, the camera an arrow below the middle, the road ridden behind
// it in the accent, the road ahead faint; north marked on the rim.
void drawAhead(QPainter &p, const gauge::Style &s, const Reading &r, const Route &route, double h)
{
    const QPointF c(h / 2.0, h / 2.0);
    const double R = kAheadFace * h;
    QPainterPath face;
    face.addEllipse(c, R, R);
    p.setPen(Qt::NoPen);
    p.setBrush(rgbColor(s.face, s.faceOpacity));
    p.drawPath(face);
    if (!r.hasValue) {
        p.setPen(rgbColor(s.text));
        p.setFont(fontAt(s, 0.16 * h, true));
        textAt(p, QStringLiteral("--"), c.x(), c.y() + 0.05 * h);
        return;
    }
    const double heading = r.position2 * std::numbers::pi / 180.0;
    const double sinH = std::sin(heading), cosH = std::cos(heading);
    const QPointF dot(c.x(), c.y() + kAheadDot * R);
    const double perM = kAheadReach * R / r.zoom; // pixels per metre
    if (r.stretch < 0 || r.stretch >= int(route.stretches.size()))
        return;
    // Only the stretch the ride is on: the next one starts where this one
    // ends along the route, but after a cut it's somewhere else.
    Route own;
    own.stretches.push_back(route.stretches[size_t(r.stretch)]);
    const QPointF here = routePoint(own, r.position);
    // East / north from the camera to forward / right of it.
    const auto px = [&](QPointF q) {
        const double dx = q.x() - here.x(), dy = q.y() - here.y();
        const double fwd = dx * sinH + dy * cosH, right = dx * cosH - dy * sinH;
        return QPointF(dot.x() + right * perM, dot.y() - fwd * perM);
    };
    p.save();
    p.setClipPath(face);
    drawRouteLine(p, own, r.position, r.alongTo, px,
                  QPen(rgbColor(s.text, 0.5), 0.03 * h, Qt::SolidLine, Qt::RoundCap,
                       Qt::RoundJoin));
    drawRouteLine(p, own, r.alongFrom, r.position, px,
                  QPen(rgbColor(s.accent), 0.035 * h, Qt::SolidLine, Qt::RoundCap,
                       Qt::RoundJoin));
    p.restore();
    // North: where it is from the camera, on the rim.
    const QPointF north(c.x() - 0.86 * R * sinH, c.y() - 0.86 * R * cosH);
    p.setPen(rgbColor(s.text, 0.9));
    p.setFont(fontAt(s, 0.075 * h, true));
    textAt(p, QStringLiteral("N"), north.x(),
           north.y() + 0.5 * QFontMetricsF(p.font()).capHeight());
    // The camera: an arrow pointing the way it travels.
    QPainterPath arrow;
    const double a = 0.05 * h;
    arrow.moveTo(dot.x(), dot.y() - 1.2 * a);
    arrow.lineTo(dot.x() + 0.85 * a, dot.y() + 0.8 * a);
    arrow.lineTo(dot.x(), dot.y() + 0.35 * a);
    arrow.lineTo(dot.x() - 0.85 * a, dot.y() + 0.8 * a);
    arrow.closeSubpath();
    p.setPen(QPen(rgbColor(s.text), 0.01 * h, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(rgbColor(s.accent));
    p.drawPath(arrow);
}

void drawRoute(QPainter &p, const gauge::Style &s, const Reading &r, const Route *route, double h)
{
    if (!route || route->empty()) {
        const bool ahead = s.routeView == gauge::RouteView::Ahead;
        p.setPen(Qt::NoPen);
        p.setBrush(rgbColor(s.face, s.faceOpacity));
        if (ahead)
            p.drawEllipse(QPointF(h / 2.0, h / 2.0), kAheadFace * h, kAheadFace * h);
        else
            p.drawRoundedRect(QRectF(0, 0, h, h), 0.12 * h, 0.12 * h);
        p.setPen(rgbColor(s.text));
        p.setFont(fontAt(s, 0.16 * h, true));
        textAt(p, QStringLiteral("--"), h / 2.0, h / 2.0 + 0.05 * h);
        return;
    }
    if (s.routeView == gauge::RouteView::Ahead)
        drawAhead(p, s, r, *route, h);
    else
        drawTrip(p, s, r, *route, h);
}

Picture draw(const gauge::Style &s, int heightPx, const Reading &r, const Route *route)
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
        case gauge::Kind::Lean: drawLean(p, s, r, w, h); break;
        case gauge::Kind::GForce: drawGForce(p, s, r, h); break;
        case gauge::Kind::Route: drawRoute(p, s, r, route, h); break;
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

double Route::alongAt(int64_t timelineMs) const
{
    double along = 0.0;
    for (const std::vector<Point> &st : stretches) {
        if (timelineMs < st.front().timelineMs)
            break;
        if (timelineMs >= st.back().timelineMs) {
            along = st.back().along;
            continue;
        }
        const auto b = std::upper_bound(st.begin(), st.end(), timelineMs,
                                        [](int64_t t, const Point &q) { return t < q.timelineMs; });
        const auto a = b - 1;
        const double f = double(timelineMs - a->timelineMs) / double(b->timelineMs - a->timelineMs);
        return a->along + f * (b->along - a->along);
    }
    return along;
}

int Route::stretchAt(int64_t timelineMs) const
{
    for (size_t i = 0; i < stretches.size(); ++i)
        if (timelineMs >= stretches[i].front().timelineMs &&
            timelineMs <= stretches[i].back().timelineMs)
            return int(i);
    return -1;
}

void Route::fitExtent()
{
    xLo = yLo = std::numeric_limits<double>::infinity();
    xHi = yHi = -std::numeric_limits<double>::infinity();
    for (const std::vector<Point> &st : stretches)
        for (const Point &q : st) {
            xLo = std::min(xLo, q.x), xHi = std::max(xHi, q.x);
            yLo = std::min(yLo, q.y), yHi = std::max(yHi, q.y);
        }
    if (stretches.empty())
        xLo = xHi = yLo = yHi = 0.0;
}

std::shared_ptr<const Route> routeOf(const title::Clock &clock)
{
    const std::vector<std::vector<title::Clock::Fix>> fixes = clock.route();
    if (fixes.empty())
        return nullptr;
    // Flat about the trip's middle latitude, in metres: east scaled by its
    // cosine, so a few hundred kilometres keep their shape. Longitudes
    // unwrapped from the first, so a trip over the date line holds together.
    const double lon0 = fixes.front().front().longitude;
    double latLo = 90.0, latHi = -90.0;
    for (const auto &st : fixes)
        for (const title::Clock::Fix &f : st) {
            latLo = std::min(latLo, f.latitude);
            latHi = std::max(latHi, f.latitude);
        }
    const double lat0 = (latLo + latHi) / 2.0;
    constexpr double kMetresPerDeg = 6371000.0 * std::numbers::pi / 180.0;
    const double east = kMetresPerDeg * std::cos(lat0 * std::numbers::pi / 180.0);

    auto route = std::make_shared<Route>();
    // Points closer than this to the last one kept add nothing a picture
    // shows; each stretch's last is kept, so the ride reaches it.
    constexpr double kMinStepM = 0.5;
    double along = 0.0;
    uint64_t key = 1469598103934665603ull; // FNV-1a
    const auto mix = [&key](const void *data, size_t n) {
        for (size_t i = 0; i < n; ++i)
            key = (key ^ static_cast<const uint8_t *>(data)[i]) * 1099511628211ull;
    };
    for (const auto &st : fixes) {
        std::vector<Route::Point> out;
        for (size_t i = 0; i < st.size(); ++i) {
            const title::Clock::Fix &f = st[i];
            Route::Point q;
            q.timelineMs = f.timelineMs;
            q.x = std::remainder(f.longitude - lon0, 360.0) * east;
            q.y = (f.latitude - lat0) * kMetresPerDeg;
            if (!out.empty()) {
                const double d = std::hypot(q.x - out.back().x, q.y - out.back().y);
                if (d < kMinStepM && i + 1 < st.size())
                    continue;
                along += d;
            }
            q.along = along;
            out.push_back(q);
            mix(&q, sizeof q);
        }
        route->stretches.push_back(std::move(out));
    }
    route->key = key;
    route->fitExtent();
    return route;
}

namespace {

// Stretch `st`'s position at `timelineMs`, held at its ends.
QPointF positionAt(const std::vector<Route::Point> &st, int64_t timelineMs)
{
    if (timelineMs <= st.front().timelineMs)
        return QPointF(st.front().x, st.front().y);
    if (timelineMs >= st.back().timelineMs)
        return QPointF(st.back().x, st.back().y);
    const auto b =
        std::upper_bound(st.begin(), st.end(), timelineMs,
                         [](int64_t t, const Route::Point &q) { return t < q.timelineMs; });
    const auto a = b - 1;
    const double f = double(timelineMs - a->timelineMs) / double(b->timelineMs - a->timelineMs);
    return QPointF(a->x + f * (b->x - a->x), a->y + f * (b->y - a->y));
}

// Which way the ride heads at `timelineMs`, radians clockwise from north:
// the chord over a few seconds either side, not the GPS's course of the
// moment, so the map turns smoothly. Standing still, the chord widens until
// it spans some movement -- the way it came and went on; a stretch that
// never moves points north.
double headingAt(const std::vector<Route::Point> &st, int64_t timelineMs)
{
    constexpr double kMinChordM = 5.0;
    for (int64_t w = 3000; w <= 120000; w *= 2) {
        const QPointF a = positionAt(st, timelineMs - w), b = positionAt(st, timelineMs + w);
        if (std::hypot(b.x() - a.x(), b.y() - a.y()) >= kMinChordM)
            return std::atan2(b.x() - a.x(), b.y() - a.y());
        if (timelineMs - w <= st.front().timelineMs && timelineMs + w >= st.back().timelineMs)
            break;
    }
    return 0.0;
}

// How far from the dot the face must reach for the road from `fromMs` to
// `toMs` to fit, seen from `timelineMs` heading `heading`: the dot sits
// below the centre, so the road ahead has more room than behind.
double reachFor(const std::vector<Route::Point> &st, int64_t timelineMs, double heading,
                int64_t fromMs, int64_t toMs)
{
    const QPointF here = positionAt(st, timelineMs);
    const double sinH = std::sin(heading), cosH = std::cos(heading);
    double need = kAheadMinM;
    const auto fit = [&](QPointF q) {
        const double dx = q.x() - here.x(), dy = q.y() - here.y();
        const double fwd = dx * sinH + dy * cosH, right = dx * cosH - dy * sinH;
        // The face's edge, kAheadReach of the way out from its centre
        // kAheadDot above the dot: the reach r where |(right, fwd - d r)|
        // = e r, d = kAheadDot / kAheadReach, e = 1 / kAheadReach.
        const double d = kAheadDot / kAheadReach, e = 1.0 / kAheadReach;
        const double qa = e * e - d * d, qb = 2.0 * d * fwd, qc = -(right * right + fwd * fwd);
        need = std::max(need, (-qb + std::sqrt(qb * qb - 4.0 * qa * qc)) / (2.0 * qa));
    };
    fit(positionAt(st, fromMs));
    fit(positionAt(st, toMs));
    auto q = std::upper_bound(st.begin(), st.end(), fromMs,
                              [](int64_t t, const Route::Point &p) { return t < p.timelineMs; });
    for (; q != st.end() && q->timelineMs < toMs; ++q)
        fit(QPointF(q->x, q->y));
    return need;
}

} // namespace

Reading readingOf(const gauge::Style &s, const Moment &moment, int heightPx, const Route *route)
{
    Reading r;
    const std::optional<Telemetry::Values> &gps = moment.gps;
    // Half a pixel's move: finer doesn't show.
    const double quantum = 0.5 / std::max(1e-9, pixelsPerUnit(s, std::max(8, heightPx)));
    const auto quantized = [quantum](double v) { return std::round(v / quantum) * quantum; };
    switch (s.kind) {
    case gauge::Kind::Route: {
        if (!route || route->empty())
            return r;
        const int64_t t = moment.timelineMs;
        const double h = std::max(8, heightPx);
        if (s.routeView == gauge::RouteView::Trip) {
            const double extent =
                std::max({route->xHi - route->xLo, route->yHi - route->yLo, 100.0});
            const double perM = (1.0 - 2.0 * kRouteMargin) * h / extent;
            r.hasValue = gps.has_value(); // the dot
            r.position = std::round(route->alongAt(t) * 2.0 * perM) / (2.0 * perM);
            return r;
        }
        // Ahead: only where the ride is on a stretch -- after a cut the road
        // ahead of the clip before isn't this clip's.
        const int k = route->stretchAt(t);
        if (k < 0 || !gps)
            return r;
        const std::vector<Route::Point> &st = route->stretches[size_t(k)];
        const auto aheadMs = int64_t(std::lround(s.aheadS * 1000.0));
        const auto behindMs = int64_t(std::lround(gauge::Style::kBehindS * 1000.0));
        const int64_t end = st.back().timelineMs, start = st.front().timelineMs;
        // The reach eased over a few seconds either side, so the face
        // doesn't zoom in and out with each bend: the most any moment
        // within 3 s needs, so the road ahead stays on the face, then its
        // log weighted by a Gaussian of 3 s. From the moments around it
        // alone, so every moment's picture is its own.
        constexpr int kStepMs = 500, kMaxOver = 6, kSmooth = 18; // in steps
        std::array<double, 2 * (kMaxOver + kSmooth) + 1> need{};
        for (int i = 0; i < int(need.size()); ++i) {
            const int64_t u =
                std::clamp(t + int64_t(i - kMaxOver - kSmooth) * kStepMs, start, end);
            need[size_t(i)] = std::log(reachFor(st, u, headingAt(st, u),
                                                std::max(start, u - behindMs),
                                                std::min(end, u + aheadMs)));
        }
        double sum = 0.0, weights = 0.0;
        for (int i = -kSmooth; i <= kSmooth; ++i) {
            const double dt = i * kStepMs;
            const double w = std::exp(-0.5 * dt * dt / (3000.0 * 3000.0));
            const auto from = need.begin() + (i + kSmooth);
            sum += w * *std::max_element(from, from + 2 * kMaxOver + 1);
            weights += w;
        }
        const double R = kAheadFace * h;
        // To half a percent, a step a picture barely shows.
        const double zoomStep = std::log(1.005);
        r.zoom = std::exp(std::round(sum / weights / zoomStep) * zoomStep);
        const double perM = kAheadReach * R / r.zoom;
        const auto toPx = [&](double along) {
            return std::round(along * 2.0 * perM) / (2.0 * perM);
        };
        r.hasValue = true;
        r.stretch = k;
        r.position = toPx(route->alongAt(t));
        r.alongFrom = toPx(route->alongAt(std::max(start, t - behindMs)));
        r.alongTo = toPx(route->alongAt(std::min(end, t + aheadMs)));
        // Half a pixel at the face's edge.
        const double degStep = 0.5 / R * 180.0 / std::numbers::pi;
        const double deg = headingAt(st, t) * 180.0 / std::numbers::pi;
        r.position2 = std::fmod(std::round(deg / degStep) * degStep + 360.0, 360.0);
        return r;
    }
    case gauge::Kind::Lean:
        if (!moment.motion)
            return r;
        r.hasValue = true;
        r.number = title::leanDegrees(moment.motion->leanDeg);
        r.position = quantized(std::clamp(moment.motion->leanDeg, -s.max, s.max));
        return r;
    case gauge::Kind::GForce: {
        const std::optional<int> g = title::gHundredths(moment.motion);
        if (!g)
            return r;
        r.hasValue = true;
        r.number = *g;
        // Past the scale the dot stays on its edge: clamped as a vector, so
        // it keeps pointing the way the force does.
        double x = moment.motion->lateralG, y = moment.motion->forwardG;
        const double len = std::hypot(x, y);
        if (len > s.max) {
            x *= s.max / len;
            y *= s.max / len;
        }
        r.position = quantized(x);
        r.position2 = quantized(y);
        return r;
    }
    case gauge::Kind::Tape:
        if (!gps || !gps->headingDeg)
            return r;
        r.hasValue = true;
        r.number = int(std::lround(*gps->headingDeg)) % 360;
        r.position = quantized(*gps->headingDeg);
        return r;
    case gauge::Kind::Dial:
    case gauge::Kind::Bar: {
        if (!gps)
            return r;
        const double v = s.reads == gauge::Reads::Speed
                             ? title::speedIn(gps->speedMps, s.units.speed)
                             : title::altitudeIn(gps->altitudeM, s.units.altitude);
        r.hasValue = true;
        r.number = int(std::lround(v));
        r.position = quantized(std::clamp(v, s.min, s.max));
        return r;
    }
    }
    return r;
}

namespace {

// Only a route gauge draws its route.
uint64_t routeKeyOf(const gauge::Style &style, const Route *route)
{
    return style.kind == gauge::Kind::Route && route ? route->key : 0;
}

} // namespace

Picture cached(const gauge::Style &style, int heightPx, const Reading &reading, const Route *route)
{
    const uint64_t routeKey = routeKeyOf(style, route);
    Cache &c = cache();
    std::lock_guard<std::mutex> lk(c.mx);
    for (auto it = c.list.begin(); it != c.list.end(); ++it)
        if (it->heightPx == heightPx && it->reading == reading && it->routeKey == routeKey &&
            it->style == style) {
            Cached hit = *it; // most recent to the front
            c.list.erase(it);
            c.list.push_front(hit);
            return hit.picture;
        }
    return {};
}

Picture render(const gauge::Style &style, int heightPx, const Reading &reading, const Route *route)
{
    if (Picture hit = cached(style, heightPx, reading, route); !hit.empty())
        return hit;
    // Drawn outside the lock: two threads may both draw the same picture,
    // and the cache then keeps one.
    const uint64_t routeKey = routeKeyOf(style, route);
    Picture pic =
        draw(style, heightPx, reading, style.kind == gauge::Kind::Route ? route : nullptr);
    Cache &c = cache();
    std::lock_guard<std::mutex> lk(c.mx);
    for (const Cached &o : c.list)
        if (o.heightPx == heightPx && o.reading == reading && o.routeKey == routeKey &&
            o.style == style)
            return o.picture;
    c.list.push_front({style, heightPx, reading, routeKey, pic});
    c.bytes += c.list.front().bytes();
    while (c.list.size() > 1 && (c.list.size() > kCacheSize || c.bytes > kCacheBytes)) {
        c.bytes -= c.list.back().bytes();
        c.list.pop_back();
    }
    return pic;
}

} // namespace gaugerender
