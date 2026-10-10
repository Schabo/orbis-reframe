#pragma once

// Tiny self-contained spectral-analysis helpers for `residual_stab --diagnose`.
// Header-only; no OpenCV / FFmpeg / Qt. Used to answer one question about the
// stick-mounted-camera shake: does the .insv IMU track actually carry the
// stick's oscillation band, or has the camera low-pass-filtered it out?

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace diag {

constexpr double kPi = std::numbers::pi;

// In-place iterative radix-2 Cooley-Tukey FFT. a.size() must be a power of two.
inline void fftRadix2(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    if (n < 2)
        return;
    for (size_t i = 1, j = 0; i < n; ++i) { // bit-reversal permutation
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

struct Spectrum {
    double binHz = 0.0;      // frequency step between adjacent bins
    double sampleRateHz = 0.0;
    size_t nUsed = 0;        // real samples before windowing / zero-pad
    std::vector<double> amp; // one-sided amplitude; bin i is frequency i*binHz
};

// Linear-detrend, Hann-window, zero-pad to a power of two, FFT, and return the
// one-sided amplitude spectrum scaled so a pure sinusoid reads ~its amplitude.
inline Spectrum amplitudeSpectrum(const std::vector<double>& x, double sampleRateHz)
{
    Spectrum s;
    s.sampleRateHz = sampleRateHz;
    s.nUsed = x.size();
    const size_t n = x.size();
    if (n < 8 || sampleRateHz <= 0.0)
        return s;

    // Least-squares linear detrend (kills DC + any ramp so it doesn't smear
    // across the low bins).
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        const double xi = static_cast<double>(i);
        sx += xi;
        sy += x[i];
        sxx += xi * xi;
        sxy += xi * x[i];
    }
    const double dn = static_cast<double>(n);
    const double denom = dn * sxx - sx * sx;
    const double slope = denom != 0.0 ? (dn * sxy - sx * sy) / denom : 0.0;
    const double intercept = (sy - slope * sx) / dn;

    std::vector<double> w(n);
    double wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / (dn - 1.0));
        wsum += w[i];
    }

    size_t m = 1;
    while (m < n)
        m <<= 1;
    m <<= 1; // one extra octave of zero-pad -> smoother bin spacing

    std::vector<std::complex<double>> a(m, {0.0, 0.0});
    for (size_t i = 0; i < n; ++i)
        a[i] = {(x[i] - (intercept + slope * static_cast<double>(i))) * w[i], 0.0};
    fftRadix2(a);

    const size_t half = m / 2;
    s.binHz = sampleRateHz / static_cast<double>(m);
    s.amp.resize(half);
    const double norm = 2.0 / wsum; // Hann coherent-gain corrected tone amplitude
    for (size_t i = 0; i < half; ++i)
        s.amp[i] = std::abs(a[i]) * norm;
    return s;
}

struct Peak {
    double freqHz = 0.0;
    double amp = 0.0;
};

inline Peak dominantPeak(const Spectrum& s, double loHz, double hiHz)
{
    Peak p;
    for (size_t i = 1; i < s.amp.size(); ++i) {
        const double f = static_cast<double>(i) * s.binHz;
        if (f < loHz || f > hiHz)
            continue;
        if (s.amp[i] > p.amp) {
            p.amp = s.amp[i];
            p.freqHz = f;
        }
    }
    return p;
}

// RMS of the signal content within [loHz, hiHz] (Parseval on the one-sided
// amplitude spectrum: each tone contributes amp^2 / 2).
inline double bandRms(const Spectrum& s, double loHz, double hiHz)
{
    double e = 0.0;
    for (size_t i = 1; i < s.amp.size(); ++i) {
        const double f = static_cast<double>(i) * s.binHz;
        if (f < loHz || f > hiHz)
            continue;
        e += 0.5 * s.amp[i] * s.amp[i];
    }
    return std::sqrt(e);
}

// Median amplitude of the top quarter of the spectrum -- a crude broadband
// noise-floor estimate.
inline double noiseFloor(const Spectrum& s)
{
    if (s.amp.size() < 16)
        return 0.0;
    std::vector<double> tail(s.amp.begin() + static_cast<std::ptrdiff_t>(s.amp.size() * 3 / 4),
                             s.amp.end());
    std::nth_element(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(tail.size() / 2),
                     tail.end());
    return tail[tail.size() / 2];
}

// Highest frequency whose *octave-band-averaged* amplitude still stands clearly
// (>= mult x) above the broadband floor -- a rough "the sensor still has real
// signal out to here". Octave-averaged (not single-bin) so one stray spike
// doesn't inflate it.
inline double bandwidthKnee(const Spectrum& s, double floor, double mult = 3.0)
{
    if (floor <= 0.0 || s.amp.size() < 8 || s.binHz <= 0.0)
        return 0.0;
    double knee = 0.0;
    for (double fc = 1.0; fc < s.sampleRateHz * 0.5; fc *= 1.4142135624) {
        const double lo = fc / 1.1892, hi = fc * 1.1892; // ~half-octave around fc
        double sum = 0.0;
        size_t n = 0;
        for (size_t i = 1; i < s.amp.size(); ++i) {
            const double f = static_cast<double>(i) * s.binHz;
            if (f < lo || f > hi)
                continue;
            sum += s.amp[i];
            ++n;
        }
        if (n && sum / static_cast<double>(n) >= mult * floor)
            knee = fc;
    }
    return knee;
}

// Pearson correlation of two equal-length series (0 if degenerate).
inline double pearson(const std::vector<double>& a, const std::vector<double>& b)
{
    const size_t n = std::min(a.size(), b.size());
    if (n < 2)
        return 0.0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma, db = b[i] - mb;
        sab += da * db;
        saa += da * da;
        sbb += db * db;
    }
    const double d = std::sqrt(saa * sbb);
    return d > 1e-20 ? sab / d : 0.0;
}

} // namespace diag
