// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SeamDepth.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>
#include <vector>

namespace insv {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

double ramp01(double x, double lo, double hi)
{
    return std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
}
} // namespace

void SeamPatchScore(const double sums[kSeamPatchSums], float& ncc, float& texture)
{
    const double* s = sums;
    const double n = s[0];
    ncc = -2.0f;
    texture = 0.0f;
    if (n < kSeamPatchAz * kSeamPatchEl / 4)
        return;
    // M = R^T R for the regressors R = (1, az, el); its inverse by cofactors.
    const double m00 = n, m01 = s[1], m02 = s[2], m11 = s[3], m12 = s[5], m22 = s[4];
    const double c00 = m11 * m22 - m12 * m12, c01 = m02 * m12 - m01 * m22,
                 c02 = m01 * m12 - m02 * m11, c11 = m00 * m22 - m02 * m02,
                 c12 = m01 * m02 - m00 * m12, c22 = m00 * m11 - m01 * m01;
    const double det = m00 * c00 + m01 * c01 + m02 * c02;
    if (std::fabs(det) < 1e-9 * n * n * n)
        return;
    // b^T M^-1 b': the part of the cross product the planes explain.
    auto proj = [&](const double* b, const double* b2) {
        const double i0 = c00 * b2[0] + c01 * b2[1] + c02 * b2[2];
        const double i1 = c01 * b2[0] + c11 * b2[1] + c12 * b2[2];
        const double i2 = c02 * b2[0] + c12 * b2[1] + c22 * b2[2];
        return (b[0] * i0 + b[1] * i1 + b[2] * i2) / det;
    };
    const double* bx = s + 6;  // sum x, x az, x el
    const double* by = s + 10; // sum y, y az, y el
    const double vx = std::max(s[9] - proj(bx, bx), 0.0) / n;
    const double vy = std::max(s[13] - proj(by, by), 0.0) / n;
    const double cxy = (s[14] - proj(bx, by)) / n;
    texture = static_cast<float>(std::sqrt(std::min(vx, vy)));
    ncc = vx > kSeamMinVariance && vy > kSeamMinVariance
              ? static_cast<float>(std::clamp(cxy / std::sqrt(vx * vy), -1.0, 1.0))
              : 0.0f;
}

void SeamPatchSample(int bin, int s, double& phi, double& e)
{
    const int az = s % kSeamPatchAz, el = s / kSeamPatchAz;
    phi = (bin + (az + 0.5) / kSeamPatchAz) * (2.0 * kPi / kSeamDepthBins);
    e = (-1.0 + (2.0 * el + 1.0) / kSeamPatchEl) * kSeamPatchElDeg * kDeg;
}

void MeasureSeamCosts(const FisheyeStitcher& st, const Image& lens0, const Image& lens1,
                      SeamCosts& out)
{
    constexpr int kSamples = kSeamPatchAz * kSeamPatchEl;
    const Image* im[2] = {&lens0, &lens1};
    auto grey = [&](int i, const Vec3& ray, double& g) {
        double u, v, theta;
        if (!st.project(i, ray, u, v, theta))
            return false;
        if (u < 0 || v < 0 || u > im[i]->w - 1.0 || v > im[i]->h - 1.0)
            return false;
        const auto c = im[i]->sample(u, v);
        g = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
        return true;
    };

    std::atomic<int> next{0};
    auto worker = [&] {
        Vec3 dirs[kSamples];
        for (int b = next.fetch_add(1); b < kSeamDepthBins; b = next.fetch_add(1)) {
            for (int s = 0; s < kSamples; ++s) {
                double phi, e;
                SeamPatchSample(b, s, phi, e);
                dirs[s] = st.seamDir(phi, e);
            }
            for (int k = 0; k < kSeamDepthCands; ++k) {
                const double q = SeamCandQ(k);
                double sums[kSeamPatchSums] = {};
                for (int s = 0; s < kSamples; ++s) {
                    double x, y;
                    if (!grey(0, st.lensRay(0, dirs[s], q), x) ||
                        !grey(1, st.lensRay(1, dirs[s], q), y))
                        continue;
                    SeamPatchAccumulate(sums, s, x, y);
                }
                SeamPatchScore(sums, out.ncc[b][k], out.texture[b][k]);
            }
        }
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < hw; ++i)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
}

SeamDepthTracker::SeamDepthTracker()
{
    reset();
}

void SeamDepthTracker::reset()
{
    std::fill(m_q, m_q + kSeamDepthBins, 0.0f);
    m_has = false;
    m_lastConf = 0;
}

void SeamDepthTracker::update(const SeamCosts& frame, int64_t ptsMs)
{
    constexpr int B = kSeamDepthBins, K = kSeamDepthCands;
    const SeamDepthTuning& T = tuning;
    if (m_has && (ptsMs < m_lastPts || ptsMs - m_lastPts > T.resetGapMs))
        reset();

    int k0 = 0; // the candidate nearest infinity: where texture is read
    for (int k = 1; k < K; ++k)
        if (std::fabs(SeamCandQ(k)) < std::fabs(SeamCandQ(k0)))
            k0 = k;
    // The candidates within the shift range, kLo..kHi (q rises with k).
    int kLo = K, kHi = -1;
    for (int k = 0; k < K; ++k) {
        const double shift = m_baselineM * SeamCandQ(k);
        if (shift >= kSeamShiftMinRad - 1e-9 && shift <= kSeamShiftMaxRad + 1e-9) {
            kLo = std::min(kLo, k);
            kHi = std::max(kHi, k);
        }
    }
    const double awayQ = T.uniqueAwayDeg * kDeg / m_baselineM;

    // This frame's data term, confidence-weighted (1 - NCC), folded into its
    // running average. Weighting before averaging matters: a frame with no
    // confidence in a bin (blown-out sky, a plain wall) adds nothing there,
    // whatever its NCC values say.
    const double keep = m_has && T.costMemoryMs > 0
                            ? std::exp(-static_cast<double>(ptsMs - m_lastPts) / T.costMemoryMs)
                            : 0.0;
    m_avgData.resize(static_cast<size_t>(B) * K);
    double confSum = 0;
    for (int b = 0; b < B; ++b) {
        double best = -2.0;
        int kb = kLo;
        for (int k = kLo; k <= kHi; ++k)
            if (std::min(frame.ncc[b][k], 1.0f) > best)
                best = std::min(frame.ncc[b][k], 1.0f), kb = k;
        // How much better it is than the best match away from it, as stereo
        // matching's uniqueness test. A best at the end of the range isn't a
        // peak -- the match goes on rising past what the table may take (a
        // ramp) -- so it counts as none.
        double away = -2.0;
        for (int k = kLo; k <= kHi; ++k)
            if (std::fabs(SeamCandQ(k) - SeamCandQ(kb)) > awayQ)
                away = std::max(away, static_cast<double>(std::min(frame.ncc[b][k], 1.0f)));
        const double unique = kb == kLo || kb == kHi ? 0.0 : best - away;
        const double conf = best < -1.5 ? 0.0
                                        : ramp01(frame.texture[b][k0], T.texLo, T.texHi) *
                                              ramp01(best, T.nccLo, T.nccHi) *
                                              ramp01(unique, T.uniqueLo, T.uniqueHi);
        confSum += conf;
        m_conf[b] = static_cast<float>(conf);
        m_unique[b] = static_cast<float>(unique);
        for (int k = 0; k < K; ++k) {
            const double n = frame.ncc[b][k] < -1.5 ? -1.0 : std::clamp(frame.ncc[b][k], -1.0f, 1.0f);
            float& a = m_avgData[static_cast<size_t>(b) * K + k];
            a = static_cast<float>(keep * a + (1 - keep) * conf * (1.0 - n));
        }
    }

    // Data term plus the far and temporal priors.
    static thread_local std::vector<double> D;
    D.assign(static_cast<size_t>(B) * K, 0.0);
    for (int b = 0; b < B; ++b)
        for (int k = 0; k < K; ++k) {
            const double q = SeamCandQ(k);
            double d = m_avgData[static_cast<size_t>(b) * K + k] + T.farPrior * std::fabs(q);
            if (k < kLo || k > kHi)
                d += 100.0; // outside the shift range: never taken
            if (m_has)
                d += T.temporal * std::fabs(q - m_q[b]);
            D[static_cast<size_t>(b) * K + k] = d;
        }
    m_lastConf = confSum / B;

    // Smoothness between neighbouring bins, truncated so depth edges stay.
    double S[K][K];
    for (int a = 0; a < K; ++a)
        for (int b = 0; b < K; ++b)
            S[a][b] = T.smooth * std::min(std::fabs(SeamCandQ(a) - SeamCandQ(b)), T.smoothCap);

    // Circular DP: run three laps round the seam and keep the middle one, so
    // every bin has converged context on both sides.
    const int N = 3 * B;
    static thread_local std::vector<double> E;
    static thread_local std::vector<uint8_t> bp;
    E.assign(static_cast<size_t>(N) * K, 0.0);
    bp.assign(static_cast<size_t>(N) * K, 0);
    for (int k = 0; k < K; ++k)
        E[k] = D[k];
    for (int i = 1; i < N; ++i) {
        const double* dPrev = &E[static_cast<size_t>(i - 1) * K];
        const double* dRow = &D[static_cast<size_t>(i % B) * K];
        double* eRow = &E[static_cast<size_t>(i) * K];
        uint8_t* bRow = &bp[static_cast<size_t>(i) * K];
        for (int k = 0; k < K; ++k) {
            double m = std::numeric_limits<double>::max();
            int arg = 0;
            for (int j = 0; j < K; ++j) {
                const double v = dPrev[j] + S[j][k];
                if (v < m)
                    m = v, arg = j;
            }
            eRow[k] = m + dRow[k];
            bRow[k] = static_cast<uint8_t>(arg);
        }
    }
    std::vector<int> label(N);
    {
        const double* last = &E[static_cast<size_t>(N - 1) * K];
        label[N - 1] = static_cast<int>(std::min_element(last, last + K) - last);
        for (int i = N - 1; i > 0; --i)
            label[i - 1] = bp[static_cast<size_t>(i) * K + label[i]];
    }

    float qNew[B];
    for (int b = 0; b < B; ++b) {
        const int k = label[B + b];
        const double* d = &D[static_cast<size_t>(b) * K];
        double off = 0;
        if (k > kLo && k < kHi) {
            const double den = d[k - 1] - 2 * d[k] + d[k + 1];
            if (den > 1e-9)
                off = std::clamp(0.5 * (d[k - 1] - d[k + 1]) / den, -0.5, 0.5);
        }
        const double step = (kSeamQMax - kSeamQMin) / (K - 1);
        qNew[b] = static_cast<float>(SeamCandQ(k) + off * step);
    }
    // A light [1 2 1] smoothing so the table has no bin-sized steps.
    float qs[B];
    for (int b = 0; b < B; ++b)
        qs[b] = 0.25f * (qNew[(b + B - 1) % B] + 2.0f * qNew[b] + qNew[(b + 1) % B]);

    const float a = m_has ? static_cast<float>(T.blend) : 1.0f;
    for (int b = 0; b < B; ++b)
        m_q[b] += a * (qs[b] - m_q[b]);
    m_has = true;
    m_lastPts = ptsMs;
}

} // namespace insv
