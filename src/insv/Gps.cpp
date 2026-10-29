// SPDX-License-Identifier: MIT OR Apache-2.0
#include "Gps.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace insv {

std::optional<double> AlignGpsBySpeed(const std::vector<SpeedSample>& fixes,
                                      const std::vector<SpeedSample>& reference) {
    // On the Antigravity A1's clips (a fix every 1.6 s) the right shift fits
    // to 0.06-0.2 m/s RMS and one second off to 0.8-2 m/s; the limits leave
    // room for noisier tracks without taking a guess.
    constexpr size_t kMinFixes = 5;
    constexpr double kMinSpeedSpreadMps = 0.5; // standard deviation of the fixes' speeds
    constexpr double kMaxRmsMps = 1.0;
    constexpr double kCoarseStepSec = 0.05, kFineStepSec = 0.001;
    constexpr double kGridSec = 0.01;
    constexpr double kOtherShiftSec = 1.0; // shifts this far from the best must fit clearly worse

    if (fixes.size() < kMinFixes || reference.size() < 2) return std::nullopt;
    double mean = 0.0;
    for (const SpeedSample& f : fixes) mean += f.speedMps;
    mean /= static_cast<double>(fixes.size());
    double var = 0.0;
    for (const SpeedSample& f : fixes) var += (f.speedMps - mean) * (f.speedMps - mean);
    if (std::sqrt(var / static_cast<double>(fixes.size())) < kMinSpeedSpreadMps)
        return std::nullopt;

    // The reference on an even grid, so each shift costs one lookup per fix.
    const double r0 = reference.front().timeSec, r1 = reference.back().timeSec;
    if (!(r1 > r0)) return std::nullopt;
    std::vector<double> grid(static_cast<size_t>((r1 - r0) / kGridSec) + 1);
    size_t j = 0;
    for (size_t i = 0; i < grid.size(); ++i) {
        const double t = r0 + static_cast<double>(i) * kGridSec;
        while (j + 2 < reference.size() && reference[j + 1].timeSec <= t) ++j;
        const SpeedSample &a = reference[j], &b = reference[j + 1];
        const double span = b.timeSec - a.timeSec;
        const double u = span > 0.0 ? std::clamp((t - a.timeSec) / span, 0.0, 1.0) : 0.0;
        grid[i] = a.speedMps + u * (b.speedMps - a.speedMps);
    }
    const size_t minOverlap = std::max(kMinFixes, fixes.size() / 2);
    const auto rms = [&](double shift) {
        double sum = 0.0;
        size_t n = 0;
        for (const SpeedSample& f : fixes) {
            const double x = (f.timeSec + shift - r0) / kGridSec;
            if (x < 0.0 || x > static_cast<double>(grid.size() - 1)) continue;
            const size_t k = std::min(static_cast<size_t>(x), grid.size() - 2);
            const double u = x - static_cast<double>(k);
            const double d = grid[k] + u * (grid[k + 1] - grid[k]) - f.speedMps;
            sum += d * d;
            ++n;
        }
        return n >= minOverlap ? std::sqrt(sum / static_cast<double>(n))
                               : std::numeric_limits<double>::quiet_NaN();
    };

    // Every shift that leaves enough fixes over the reference, coarsely,
    // then the best one to the millisecond.
    const double lo = r0 - fixes.back().timeSec, hi = r1 - fixes.front().timeSec;
    std::vector<std::pair<double, double>> coarse; // shift, rms
    for (double s = lo; s <= hi; s += kCoarseStepSec)
        if (const double e = rms(s); std::isfinite(e)) coarse.emplace_back(s, e);
    if (coarse.empty()) return std::nullopt;
    double best = std::min_element(coarse.begin(), coarse.end(), [](const auto& a, const auto& b) {
                      return a.second < b.second;
                  })->first;
    double bestRms = rms(best);
    for (double s = best - kCoarseStepSec; s <= best + kCoarseStepSec; s += kFineStepSec)
        if (const double e = rms(s); e < bestRms) best = s, bestRms = e;
    if (!(bestRms <= kMaxRmsMps)) return std::nullopt;

    bool compared = false;
    for (const auto& [s, e] : coarse) {
        if (std::fabs(s - best) < kOtherShiftSec) continue;
        if (e < 2.0 * bestRms + 0.1) return std::nullopt;
        compared = true;
    }
    if (!compared) return std::nullopt;
    return best;
}

} // namespace insv
