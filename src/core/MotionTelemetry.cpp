#include "MotionTelemetry.h"

#include <cmath>

MotionTelemetry::MotionTelemetry(insv::MotionTrack track) : m_track(std::move(track)) {}

std::optional<MotionTelemetry::Values> MotionTelemetry::sample(size_t i) const
{
    const Values v{m_track.leanDeg[i], m_track.lateralG[i], m_track.forwardG[i]};
    if (!std::isfinite(v.leanDeg) || !std::isfinite(v.lateralG) || !std::isfinite(v.forwardG))
        return std::nullopt;
    return v;
}

std::optional<MotionTelemetry::Values> MotionTelemetry::at(double sourceMs) const
{
    const size_t n = m_track.size();
    if (n == 0 || !(m_track.stepSec > 0.0))
        return std::nullopt;
    const double x = sourceMs / (m_track.stepSec * 1000.0); // in steps
    if (x < -1.0 || x > double(n)) // past the held step
        return std::nullopt;
    if (x <= 0.0)
        return sample(0);
    if (x >= double(n - 1))
        return sample(n - 1);
    const size_t i = size_t(x);
    const double f = x - double(i);
    const auto a = sample(i), b = sample(i + 1);
    // Next to a gap, the nearer side if it has data.
    if (!a || !b)
        return f < 0.5 ? a : b;
    return Values{a->leanDeg + f * (b->leanDeg - a->leanDeg),
                  a->lateralG + f * (b->lateralG - a->lateralG),
                  a->forwardG + f * (b->forwardG - a->forwardG)};
}
