#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

// Opt-in timing log for playback checks: with ORBIS_FRAME_STATS=1 set,
// the viewport and the playback presenter each print a one-line summary to
// stderr about once a second. Off (and nearly free) otherwise.
namespace render::framestats {

inline bool Enabled()
{
    static const bool on = [] {
        const char *v = std::getenv("ORBIS_FRAME_STATS");
        return v && *v && *v != '0';
    }();
    return on;
}

using Clock = std::chrono::steady_clock;

inline double MsBetween(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Samples in ms over one report window.
struct Samples {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    void clear() { v.clear(); }
    size_t count() const { return v.size(); }
    double mean() const
    {
        double s = 0;
        for (double x : v)
            s += x;
        return v.empty() ? 0.0 : s / v.size();
    }
    double max() const { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); }
    double pct(double p) const
    {
        if (v.empty())
            return 0.0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        return s[std::min(s.size() - 1, static_cast<size_t>(p * (s.size() - 1) + 0.5))];
    }
};

} // namespace render::framestats
