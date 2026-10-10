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
                double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
                int n = 0;
                for (int s = 0; s < kSamples; ++s) {
                    double x, y;
                    if (!grey(0, st.lensRay(0, dirs[s], q), x) ||
                        !grey(1, st.lensRay(1, dirs[s], q), y))
                        continue;
                    sx += x, sy += y, sxx += x * x, syy += y * y, sxy += x * y;
                    ++n;
                }
                float ncc = -2.0f, tex = 0.0f;
                if (n >= kSamples / 4) {
                    const double vx = std::max(sxx / n - (sx / n) * (sx / n), 0.0);
                    const double vy = std::max(syy / n - (sy / n) * (sy / n), 0.0);
                    const double cxy = sxy / n - (sx / n) * (sy / n);
                    tex = static_cast<float>(std::sqrt(std::min(vx, vy)));
                    ncc = vx > 1e-6 && vy > 1e-6 ? static_cast<float>(cxy / std::sqrt(vx * vy))
                                                 : 0.0f;
                }
                out.ncc[b][k] = ncc;
                out.texture[b][k] = tex;
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

void SeamDepthTracker::update(const SeamCosts& c, int64_t ptsMs)
{
    constexpr int B = kSeamDepthBins, K = kSeamDepthCands;
    const SeamDepthTuning& T = tuning;
    if (m_has && (ptsMs < m_lastPts || ptsMs - m_lastPts > T.resetGapMs))
        reset();

    int k0 = 0; // the candidate nearest infinity: where texture is read
    for (int k = 1; k < K; ++k)
        if (std::fabs(SeamCandQ(k)) < std::fabs(SeamCandQ(k0)))
            k0 = k;

    // Data term: confidence-weighted (1 - NCC), plus the far and temporal priors.
    static thread_local std::vector<double> D;
    D.assign(static_cast<size_t>(B) * K, 0.0);
    double confSum = 0;
    for (int b = 0; b < B; ++b) {
        double best = -2.0;
        for (int k = 0; k < K; ++k)
            best = std::max(best, static_cast<double>(c.ncc[b][k]));
        const double conf = best < -1.5 ? 0.0
                                        : ramp01(c.texture[b][k0], T.texLo, T.texHi) *
                                              ramp01(best, T.nccLo, T.nccHi);
        confSum += conf;
        for (int k = 0; k < K; ++k) {
            const double q = SeamCandQ(k);
            const double ncc = c.ncc[b][k] < -1.5 ? -1.0 : c.ncc[b][k];
            double d = conf * (1.0 - ncc) + T.farPrior * std::fabs(q);
            if (m_has)
                d += T.temporal * std::fabs(q - m_q[b]);
            D[static_cast<size_t>(b) * K + k] = d;
        }
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
        if (k > 0 && k < K - 1) {
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
