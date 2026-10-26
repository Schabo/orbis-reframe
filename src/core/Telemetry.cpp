#include "Telemetry.h"

#include <algorithm>
#include <cmath>

namespace {

// The angle `a` moved by whole turns to within half a turn of `near`.
double unwrapNear(double a, double near)
{
    return a - 360.0 * std::round((a - near) / 360.0);
}

// Fritsch-Carlson slopes of a monotone cubic through (t[i], y[i]), i in
// [first, last]: the weighted harmonic mean of the neighbouring secants, 0
// at a peak or a dip; at the ends a three-point estimate kept from
// overshooting.
void monotoneSlopes(const std::vector<double> &t, const std::vector<double> &y, size_t first,
                    size_t last, std::vector<double> &d)
{
    const size_t n = last - first + 1;
    if (n == 1) {
        d[first] = 0.0;
        return;
    }
    auto h = [&](size_t k) { return t[k + 1] - t[k]; };
    auto delta = [&](size_t k) { return (y[k + 1] - y[k]) / h(k); };
    if (n == 2) {
        d[first] = d[last] = delta(first);
        return;
    }
    for (size_t k = first + 1; k < last; ++k) {
        const double d0 = delta(k - 1), d1 = delta(k);
        if (d0 * d1 <= 0.0) {
            d[k] = 0.0;
            continue;
        }
        const double w1 = 2.0 * h(k) + h(k - 1), w2 = h(k) + 2.0 * h(k - 1);
        d[k] = (w1 + w2) / (w1 / d0 + w2 / d1);
    }
    auto end = [](double h0, double h1, double d0, double d1) {
        double s = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
        if (s * d0 <= 0.0)
            s = 0.0;
        else if (d0 * d1 < 0.0 && std::abs(s) > 3.0 * std::abs(d0))
            s = 3.0 * d0;
        return s;
    };
    d[first] = end(h(first), h(first + 1), delta(first), delta(first + 1));
    d[last] = end(h(last - 1), h(last - 2), delta(last - 1), delta(last - 2));
}

} // namespace

Telemetry::Telemetry(const insv::GpsTrack &track)
{
    for (const insv::GpsFix &f : track.fixes) {
        if (!m_points.empty() && f.timeMs <= m_points.back().timeMs)
            continue;
        Point p;
        p.timeMs = double(f.timeMs);
        p.value = {f.latitude, f.longitude, std::max(0.0, f.speedMps), f.headingDeg, f.altitudeM};
        p.hasHeading = std::isfinite(f.headingDeg);
        if (!m_points.empty())
            m_points.back().segmentEnd = p.timeMs - m_points.back().timeMs > double(kMaxGapMs);
        m_points.push_back(p);
    }
    if (m_points.empty())
        return;
    m_points.back().segmentEnd = true;

    std::vector<double> t(m_points.size()), y(m_points.size()), d(m_points.size());
    for (size_t i = 0; i < m_points.size(); ++i)
        t[i] = m_points[i].timeMs;
    for (size_t first = 0; first < m_points.size();) {
        size_t last = first;
        while (!m_points[last].segmentEnd)
            ++last;
        // A heading the camera didn't know: the one before it, or at the
        // start the first one known.
        const Point *known = nullptr;
        for (size_t i = first; i <= last && !known; ++i)
            if (m_points[i].hasHeading)
                known = &m_points[i];
        for (size_t i = first; i <= last; ++i) {
            Point &p = m_points[i];
            if (p.hasHeading)
                known = &p;
            else if (known)
                p.value[3] = known->value[3];
            p.hasHeading = known != nullptr;
            if (!p.hasHeading)
                p.value[3] = 0.0;
        }
        // Each step of an angle the short way round.
        for (size_t i = first + 1; i <= last; ++i)
            for (size_t c : {size_t(1), size_t(3)})
                m_points[i].value[c] = unwrapNear(m_points[i].value[c], m_points[i - 1].value[c]);
        for (size_t c = 0; c < kChannels; ++c) {
            for (size_t i = first; i <= last; ++i)
                y[i] = m_points[i].value[c];
            monotoneSlopes(t, y, first, last, d);
            for (size_t i = first; i <= last; ++i)
                m_points[i].slope[c] = d[i];
        }
        first = last + 1;
    }
}

Telemetry::Values Telemetry::valuesOf(const std::array<double, kChannels> &v, bool hasHeading)
{
    Values out;
    out.latitude = v[0];
    out.longitude = std::remainder(v[1], 360.0);
    out.speedMps = std::max(0.0, v[2]);
    if (hasHeading)
        out.headingDeg = std::fmod(std::fmod(v[3], 360.0) + 360.0, 360.0);
    out.altitudeM = v[4];
    return out;
}

std::optional<Telemetry::Values> Telemetry::at(double sourceMs) const
{
    if (m_points.empty())
        return std::nullopt;
    // The first point after sourceMs.
    const auto next = std::upper_bound(m_points.begin(), m_points.end(), sourceMs,
                                       [](double ms, const Point &p) { return ms < p.timeMs; });
    if (next == m_points.begin()) {
        if (next->timeMs - sourceMs <= double(kHoldMs))
            return valuesOf(next->value, next->hasHeading);
        return std::nullopt;
    }
    const Point &a = *std::prev(next);
    if (next == m_points.end() || a.segmentEnd) {
        if (sourceMs - a.timeMs <= double(kHoldMs))
            return valuesOf(a.value, a.hasHeading);
        if (next != m_points.end() && next->timeMs - sourceMs <= double(kHoldMs))
            return valuesOf(next->value, next->hasHeading);
        return std::nullopt;
    }
    const Point &b = *next;
    const double h = b.timeMs - a.timeMs;
    const double s = (sourceMs - a.timeMs) / h;
    const double s2 = s * s, s3 = s2 * s;
    const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s;
    const double h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
    std::array<double, kChannels> v{};
    for (size_t c = 0; c < v.size(); ++c)
        v[c] = h00 * a.value[c] + h10 * h * a.slope[c] + h01 * b.value[c] + h11 * h * b.slope[c];
    return valuesOf(v, a.hasHeading);
}
