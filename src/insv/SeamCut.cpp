// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SeamCut.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>

namespace insv {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;
constexpr int kCutSamples = kSeamCutPatchAz * kSeamCutPatchEl;
// The disagreement of a cell the lenses don't both see: never chosen while
// another row is usable, and the full cross-fade where none is.
constexpr double kNoCell = 2.0;

double smoothstep(double lo, double hi, double x)
{
    const double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
} // namespace

double SeamCutRowMax(double bandRad)
{
    return std::max(bandRad - (kSeamCutFadeDeg + 0.5) * kDeg, 0.0);
}

double SeamCutRowE(int r, double bandRad)
{
    const double m = SeamCutRowMax(bandRad);
    return -m + 2.0 * m * r / (kSeamCutRows - 1);
}

void SeamCutSample(int bin, int row, int s, double bandRad, double& phi, double& e)
{
    const int az = s % kSeamCutPatchAz, el = s / kSeamCutPatchAz;
    phi = (bin + (az + 0.5) / kSeamCutPatchAz) * (2.0 * kPi / kSeamDepthBins);
    e = SeamCutRowE(row, bandRad) +
        (-1.0 + (2.0 * el + 1.0) / kSeamCutPatchEl) * kSeamCutPatchElDeg * kDeg;
}

void MeasureSeamCutStats(const FisheyeStitcher& st, const Image& lens0, const Image& lens1,
                         const float* seamQ, SeamCutStats& out)
{
    const double band = st.gpuParams().bandRad;
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

    constexpr int kCells = kSeamDepthBins * kSeamCutRows;
    std::atomic<int> next{0};
    auto worker = [&] {
        double g0[kCutSamples], g1[kCutSamples];
        for (int c = next.fetch_add(1); c < kCells; c = next.fetch_add(1)) {
            const int b = c / kSeamCutRows, r = c % kSeamCutRows;
            int n = 0;
            double s0 = 0, s1 = 0;
            for (int s = 0; s < kCutSamples; ++s) {
                double phi, e;
                SeamCutSample(b, r, s, band, phi, e);
                const Vec3 dir = st.seamDir(phi, e);
                const double q = st.seamQAt(dir, seamQ);
                if (!grey(0, st.lensRay(0, dir, q), g0[n]) || !grey(1, st.lensRay(1, dir, q), g1[n]))
                    continue;
                s0 += g0[n], s1 += g1[n];
                ++n;
            }
            SeamCutStats::Cell& cell = out.cell[b][r];
            cell = SeamCutStats::Cell{};
            cell.n = static_cast<float>(n);
            if (n == 0)
                continue;
            const double m0 = s0 / n, m1 = s1 / n;
            double sad = 0, v0 = 0, v1 = 0;
            for (int i = 0; i < n; ++i) {
                const double d0 = g0[i] - m0, d1 = g1[i] - m1;
                sad += std::fabs(d0 - d1);
                v0 += d0 * d0, v1 += d1 * d1;
            }
            cell.zsad = static_cast<float>(sad / n);
            cell.texture = static_cast<float>(std::sqrt(std::min(v0, v1) / n));
        }
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < hw; ++i)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
}

SeamCutTracker::SeamCutTracker()
{
    reset();
}

void SeamCutTracker::reset()
{
    m_blend = SeamBlend{};
    m_has = false;
    m_avgDis.clear();
    m_row.assign(kSeamDepthBins, -1);
    m_dis.assign(kSeamDepthBins, 0.0f);
    m_width.assign(kSeamDepthBins, 1.0f);
}

void SeamCutTracker::update(const SeamCutStats& stats, int64_t ptsMs, double bandRad)
{
    constexpr int B = kSeamDepthBins, R = kSeamCutRows;
    const SeamCutTuning& T = tuning;
    if (m_has && (ptsMs < m_lastPts || ptsMs - m_lastPts > T.resetGapMs))
        reset();

    // This frame's disagreement per cell -- the zero-mean difference against
    // the texture, so a busy patch that matches isn't taken for an occlusion
    // -- folded into its running average.
    const double keep =
        m_has && T.memoryMs > 0 ? std::exp(-static_cast<double>(ptsMs - m_lastPts) / T.memoryMs)
                                : 0.0;
    m_avgDis.resize(static_cast<size_t>(B) * R);
    for (int b = 0; b < B; ++b)
        for (int r = 0; r < R; ++r) {
            const SeamCutStats::Cell& c = stats.cell[b][r];
            const double d = c.n >= kCutSamples / 2
                                 ? std::min(c.zsad / (c.texture + T.texFloor), kNoCell)
                                 : kNoCell;
            float& a = m_avgDis[static_cast<size_t>(b) * R + r];
            a = static_cast<float>(keep * a + (1.0 - keep) * d);
        }

    // The cut: one row per bin, by a circular DP along the azimuth (three
    // laps, the middle one kept, as SeamDepthTracker).
    double rowDeg[R];
    for (int r = 0; r < R; ++r)
        rowDeg[r] = SeamCutRowE(r, bandRad) / kDeg;
    auto unary = [&](int b, int r) {
        double u = m_avgDis[static_cast<size_t>(b) * R + r] + T.offCentre * std::fabs(rowDeg[r]);
        if (m_has && m_row[b] >= 0 && m_row[b] != r)
            u += T.stay;
        return u;
    };
    const int N = 3 * B;
    std::vector<double> E(static_cast<size_t>(N) * R);
    std::vector<uint8_t> bp(static_cast<size_t>(N) * R, 0);
    for (int r = 0; r < R; ++r)
        E[r] = unary(0, r);
    for (int i = 1; i < N; ++i)
        for (int r = 0; r < R; ++r) {
            double m = std::numeric_limits<double>::max();
            int arg = 0;
            for (int j = 0; j < R; ++j) {
                const double v =
                    E[static_cast<size_t>(i - 1) * R + j] + T.rowChange * std::fabs(rowDeg[r] - rowDeg[j]);
                if (v < m)
                    m = v, arg = j;
            }
            E[static_cast<size_t>(i) * R + r] = m + unary(i % B, r);
            bp[static_cast<size_t>(i) * R + r] = static_cast<uint8_t>(arg);
        }
    std::vector<int> label(N);
    {
        const double* last = &E[static_cast<size_t>(N - 1) * R];
        label[N - 1] = static_cast<int>(std::min_element(last, last + R) - last);
        for (int i = N - 1; i > 0; --i)
            label[i - 1] = bp[static_cast<size_t>(i) * R + label[i]];
    }

    // The fade's width from the disagreement at the chosen row, smoothed
    // along the azimuth so a wide stretch doesn't start abruptly; its centre
    // moves to the seam circle as it widens, so it stays inside the band.
    for (int b = 0; b < B; ++b) {
        m_row[b] = label[B + b];
        m_dis[b] = m_avgDis[static_cast<size_t>(b) * R + m_row[b]];
    }
    const double narrow = std::min(kSeamCutFadeDeg * kDeg, bandRad);
    const int reach = static_cast<int>(std::ceil(2.0 * T.widthSigma));
    for (int b = 0; b < B; ++b) {
        double a = 0, w = 0;
        for (int d = -reach; d <= reach; ++d) {
            const double ww = T.widthSigma > 0 ? std::exp(-0.5 * d * d / (T.widthSigma * T.widthSigma))
                                               : (d == 0 ? 1.0 : 0.0);
            a += ww * smoothstep(T.agreeLo, T.agreeHi, m_dis[(b + d + B) % B]);
            w += ww;
        }
        const double s = a / w;
        m_width[b] = static_cast<float>(s);
        m_blend.cut[b] = static_cast<float>(SeamCutRowE(m_row[b], bandRad) * (1.0 - s));
        m_blend.fade[b] = static_cast<float>(narrow + (bandRad - narrow) * s);
    }
    m_has = true;
    m_lastPts = ptsMs;
}

} // namespace insv
