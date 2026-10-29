// SPDX-License-Identifier: MIT OR Apache-2.0
#include "Gps.h"

#include "Smooth.h"

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

double GpsClockFit::shiftAt(double videoSec) const {
    if (shifts.empty()) return 0.0;
    if (videoSec <= shifts.front().first) return shifts.front().second;
    if (videoSec >= shifts.back().first) return shifts.back().second;
    const auto b = std::upper_bound(shifts.begin(), shifts.end(), videoSec,
                                    [](double t, const auto& k) { return t < k.first; });
    const auto a = b - 1;
    const double u = (videoSec - a->first) / (b->first - a->first);
    return a->second + u * (b->second - a->second);
}

int64_t GpsClockFit::videoMs(int64_t timeMs) const {
    return timeMs - std::llround(shiftAt(static_cast<double>(timeMs) * 1e-3) * 1e3);
}

std::optional<GpsClockFit> AlignGpsByAcceleration(const GpsTrack& track,
                                                  const std::vector<float>& forwardG,
                                                  double stepSec) {
    // Measured on the X5 samples (1 s smoothing on both sides): the best
    // shift matches at r 0.81-0.99 per 2 min window where the speed changes
    // (0.016-0.07 g RMS), and at r 0.02 standing still.
    constexpr double kG = 9.80665;
    constexpr int64_t kMaxGapMs = 3000;   // as Telemetry: farther apart is no data
    constexpr double kSpeedSmoothSec = 0.5, kAccelSmoothSec = 1.0;
    constexpr double kMaxShiftSec = 3.0;
    constexpr double kCoarseStepSec = 0.05;
    constexpr double kWindowSec = 120.0;
    constexpr double kMinOverlapSec = 20.0;
    constexpr double kMinR = 0.6;
    constexpr double kMinAccelRmsG = 0.01; // the GPS's change of speed, in a window
    // A window this far from the median of all is taken as a wrong match.
    constexpr double kMaxFromMedianSec = 0.75;

    const size_t n = forwardG.size();
    const auto& fixes = track.fixes;
    if (n < 3 || !(stepSec > 0.0) || fixes.size() < 2) return std::nullopt;
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    // The fixes' speed on the motion's grid, straight between fixes.
    std::vector<double> speed(n, kNaN);
    size_t f = 0;
    for (size_t i = 0; i < n; ++i) {
        const double ms = static_cast<double>(i) * stepSec * 1e3;
        while (f + 2 < fixes.size() && static_cast<double>(fixes[f + 1].timeMs) <= ms) ++f;
        const GpsFix &a = fixes[f], &b = fixes[f + 1];
        if (ms < static_cast<double>(a.timeMs) || ms > static_cast<double>(b.timeMs) ||
            b.timeMs - a.timeMs > kMaxGapMs)
            continue;
        const double span = static_cast<double>(b.timeMs - a.timeMs);
        const double u = span > 0.0 ? (ms - static_cast<double>(a.timeMs)) / span : 0.0;
        speed[i] = a.speedMps + u * (b.speedMps - a.speedMps);
    }
    speed = GaussianSmooth(speed, stepSec, kSpeedSmoothSec);
    std::vector<double> gps(n, kNaN);
    for (size_t i = 1; i + 1 < n; ++i)
        if (!std::isnan(speed[i - 1]) && !std::isnan(speed[i + 1]))
            gps[i] = (speed[i + 1] - speed[i - 1]) / (2.0 * stepSec) / kG;
    gps = GaussianSmooth(gps, stepSec, kAccelSmoothSec);
    const std::vector<double> imu = GaussianSmooth(
        std::vector<double>(forwardG.begin(), forwardG.end()), stepSec, kAccelSmoothSec);

    size_t first = n, last = 0;
    for (size_t i = 0; i < n; ++i)
        if (!std::isnan(gps[i])) first = std::min(first, i), last = i;
    if (first >= last) return std::nullopt;

    const long maxShift = std::lround(kMaxShiftSec / stepSec);
    const long coarse = std::max(1L, std::lround(kCoarseStepSec / stepSec));
    const size_t minOverlap = static_cast<size_t>(kMinOverlapSec / stepSec);
    // The IMU at i against the GPS at i + shift, over [w0, w1).
    const auto correlate = [&](size_t w0, size_t w1, long shift, double* gpsRms) {
        double sa = 0, sb = 0, sab = 0, saa = 0, sbb = 0;
        size_t c = 0;
        for (size_t i = w0; i < w1; ++i) {
            const long k = static_cast<long>(i) + shift;
            if (k < 0 || k >= static_cast<long>(n)) continue;
            const double a = imu[i], b = gps[static_cast<size_t>(k)];
            if (std::isnan(a) || std::isnan(b)) continue;
            sa += a, sb += b, sab += a * b, saa += a * a, sbb += b * b;
            ++c;
        }
        if (c < minOverlap) return kNaN;
        const double ma = sa / c, mb = sb / c;
        const double va = saa / c - ma * ma, vb = sbb / c - mb * mb;
        if (gpsRms) *gpsRms = std::sqrt(std::max(vb, 0.0));
        return va > 0.0 && vb > 0.0 ? (sab / c - ma * mb) / std::sqrt(va * vb) : kNaN;
    };

    struct Match { double centreSec, shiftSec; };
    std::vector<Match> matches;
    const size_t length = last - first + 1;
    const size_t windows = std::max<size_t>(
        1, static_cast<size_t>(static_cast<double>(length) * stepSec / kWindowSec));
    for (size_t w = 0; w < windows; ++w) {
        const size_t w0 = first + length * w / windows, w1 = first + length * (w + 1) / windows;
        long best = 0;
        double bestR = -2.0;
        for (long sft = -maxShift; sft <= maxShift; sft += coarse)
            if (const double r = correlate(w0, w1, sft, nullptr); r > bestR) best = sft, bestR = r;
        for (long sft = best - coarse; sft <= best + coarse; ++sft)
            if (const double r = correlate(w0, w1, sft, nullptr); r > bestR) best = sft, bestR = r;
        double gpsRms = 0.0;
        correlate(w0, w1, best, &gpsRms);
        if (!(bestR >= kMinR) || gpsRms < kMinAccelRmsG || std::labs(best) > maxShift - coarse)
            continue;
        matches.push_back({(static_cast<double>(w0 + w1) / 2.0) * stepSec,
                           static_cast<double>(best) * stepSec});
    }
    if (matches.empty()) return std::nullopt;

    std::vector<double> sorted;
    for (const Match& m : matches) sorted.push_back(m.shiftSec);
    std::sort(sorted.begin(), sorted.end());
    const size_t h = sorted.size() / 2;
    const double median = sorted.size() % 2 ? sorted[h] : (sorted[h - 1] + sorted[h]) / 2.0;
    GpsClockFit fit;
    for (const Match& m : matches)
        if (std::fabs(m.shiftSec - median) <= kMaxFromMedianSec)
            fit.shifts.emplace_back(m.centreSec, m.shiftSec);
    if (fit.shifts.empty()) return std::nullopt; // two windows, far apart
    return fit;
}

} // namespace insv
