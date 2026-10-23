// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SeamCut.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>
#include <utility>
#include <vector>

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
    auto colour = [&](int i, const Vec3& ray, double rgb[3], double& g) {
        double u, v, theta;
        if (!st.project(i, ray, u, v, theta))
            return false;
        if (u < 0 || v < 0 || u > im[i]->w - 1.0 || v > im[i]->h - 1.0)
            return false;
        const auto c = im[i]->sample(u, v);
        for (int k = 0; k < 3; ++k)
            rgb[k] = c[k];
        g = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
        return true;
    };

    constexpr int kCells = kSeamDepthBins * kSeamCutRows;
    std::atomic<int> next{0};
    auto worker = [&] {
        double g0[kCutSamples], g1[kCutSamples];
        for (int c = next.fetch_add(1); c < kCells; c = next.fetch_add(1)) {
            const int b = c / kSeamCutRows, r = c % kSeamCutRows;
            int n = 0, nc = 0;
            double s0 = 0, s1 = 0, m[2][3] = {};
            for (int s = 0; s < kCutSamples; ++s) {
                double phi, e, c0[3], c1[3];
                SeamCutSample(b, r, s, band, phi, e);
                const Vec3 dir = st.seamDir(phi, e);
                const double q = st.seamQAt(dir, seamQ);
                if (!colour(0, st.lensRay(0, dir, q), c0, g0[n]) ||
                    !colour(1, st.lensRay(1, dir, q), c1, g1[n]))
                    continue;
                s0 += g0[n], s1 += g1[n];
                ++n;
                if (std::max({c0[0], c0[1], c0[2], c1[0], c1[1], c1[2]}) > kSeamCutClip)
                    continue;
                for (int k = 0; k < 3; ++k)
                    m[0][k] += c0[k], m[1][k] += c1[k];
                ++nc;
            }
            SeamCutStats::Cell& cell = out.cell[b][r];
            cell = SeamCutStats::Cell{};
            cell.n = static_cast<float>(n);
            cell.nColour = static_cast<float>(nc);
            for (int i = 0; i < 2 && nc > 0; ++i)
                for (int k = 0; k < 3; ++k)
                    cell.mean[i][k] = static_cast<float>(m[i][k] / nc);
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
    m_gainHas = false;
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
    updateGain(stats, ptsMs);
    m_has = true;
    m_lastPts = ptsMs;
}

void SeamCutTracker::updateGain(const SeamCutStats& stats, int64_t ptsMs)
{
    constexpr int B = kSeamDepthBins, R = kSeamCutRows;
    const SeamCutTuning& T = tuning;

    // Per bin: both lenses' mean colour over the rows they agree on, as
    // log(lens 0 / lens 1).
    double logRatio[B][3] = {}, weight[B] = {};
    for (int b = 0; b < B; ++b) {
        double m[2][3] = {}, ws = 0;
        for (int r = 0; r < R; ++r) {
            const SeamCutStats::Cell& c = stats.cell[b][r];
            if (c.nColour < kCutSamples / 2)
                continue;
            const double w = 1.0 - smoothstep(T.agreeLo, T.agreeHi,
                                              m_avgDis[static_cast<size_t>(b) * R + r]);
            for (int i = 0; i < 2; ++i)
                for (int k = 0; k < 3; ++k)
                    m[i][k] += w * c.mean[i][k];
            ws += w;
        }
        if (ws < 0.5)
            continue;
        bool ok = true;
        for (int i = 0; i < 2; ++i)
            for (int k = 0; k < 3; ++k)
                ok = ok && m[i][k] > T.gainMinLevel * ws;
        if (!ok)
            continue;
        weight[b] = ws;
        for (int k = 0; k < 3; ++k)
            logRatio[b][k] = std::log(m[0][k] / m[1][k]);
    }

    // A smooth curve round the seam per channel -- a constant and the first
    // gainHarmonics harmonics of the azimuth -- by weighted least squares,
    // refitted with the bins that stray far from it down-weighted (Tukey).
    // An exposure or white-balance difference between the lenses changes
    // slowly round the seam; a shiny surface that reflects something else
    // into each lens (a tank under the camera) is a few bins of ratios far
    // off, and must not darken or brighten that stretch. The harmonics are
    // pulled lightly towards 0, so a stretch with nothing to go by gets the
    // overall match rather than a swing.
    const int H = std::clamp(T.gainHarmonics, 0, 4), P = 1 + 2 * H;
    double basis[B][9];
    int bins = 0;
    for (int b = 0; b < B; ++b) {
        const double phi = (b + 0.5) * 2.0 * kPi / B;
        basis[b][0] = 1.0;
        for (int h = 1; h <= H; ++h) {
            basis[b][2 * h - 1] = std::cos(h * phi);
            basis[b][2 * h] = std::sin(h * phi);
        }
        bins += weight[b] > 0 ? 1 : 0;
    }
    const bool fitted = bins >= T.gainMinBins;
    double coef[3][9] = {};
    if (fitted) {
        double robust[B];
        std::fill(robust, robust + B, 1.0);
        for (int round = 0; round < 6; ++round) {
            double A[9][9] = {}, rhs[3][9] = {}, wAll = 0;
            for (int b = 0; b < B; ++b) {
                const double w = weight[b] * robust[b];
                if (w <= 0)
                    continue;
                wAll += w;
                for (int i = 0; i < P; ++i) {
                    for (int j = 0; j < P; ++j)
                        A[i][j] += w * basis[b][i] * basis[b][j];
                    for (int k = 0; k < 3; ++k)
                        rhs[k][i] += w * basis[b][i] * logRatio[b][k];
                }
            }
            for (int i = 1; i < P; ++i)
                A[i][i] += T.gainRidge * wAll;
            // Gaussian elimination (A is symmetric positive definite).
            for (int i = 0; i < P; ++i)
                for (int r = i + 1; r < P; ++r) {
                    const double f = A[r][i] / A[i][i];
                    for (int j = i; j < P; ++j)
                        A[r][j] -= f * A[i][j];
                    for (int k = 0; k < 3; ++k)
                        rhs[k][r] -= f * rhs[k][i];
                }
            for (int k = 0; k < 3; ++k)
                for (int i = P - 1; i >= 0; --i) {
                    double v = rhs[k][i];
                    for (int j = i + 1; j < P; ++j)
                        v -= A[i][j] * coef[k][j];
                    coef[k][i] = v / A[i][i];
                }
            // Each bin's worst channel against the curve; the scale from
            // their weighted median, never below the noise.
            std::vector<std::pair<double, double>> res;
            double resOf[B] = {};
            for (int b = 0; b < B; ++b) {
                if (weight[b] <= 0)
                    continue;
                for (int k = 0; k < 3; ++k) {
                    double f = 0;
                    for (int i = 0; i < P; ++i)
                        f += coef[k][i] * basis[b][i];
                    resOf[b] = std::max(resOf[b], std::fabs(logRatio[b][k] - f));
                }
                res.emplace_back(resOf[b], weight[b]);
            }
            std::sort(res.begin(), res.end());
            double half = 0, med = 0;
            for (const auto& [r, w] : res)
                half += w;
            half *= 0.5;
            for (const auto& [r, w] : res)
                if ((half -= w) <= 0) {
                    med = r;
                    break;
                }
            const double c = 4.685 * std::max(T.gainNoise, 1.4826 * med);
            for (int b = 0; b < B; ++b) {
                const double u = resOf[b] / c;
                robust[b] = u >= 1.0 ? 0.0 : (1 - u * u) * (1 - u * u);
            }
        }
    }

    // Then over time; without a fit the match fades to the photometric gain.
    const double keep = m_gainHas && T.gainMemoryMs > 0
                            ? std::exp(-static_cast<double>(ptsMs - m_lastPts) / T.gainMemoryMs)
                            : 0.0;
    const double maxLog = 2.0 * std::log(T.gainMax);
    for (int b = 0; b < B; ++b) {
        float* g = m_blend.gain[b];
        for (int k = 0; k < 3; ++k) {
            double f = 0;
            for (int i = 0; i < P; ++i)
                f += coef[k][i] * basis[b][i];
            const double target = fitted ? std::clamp(f, -maxLog, maxLog) : g[k];
            g[k] = static_cast<float>(keep * g[k] + (1.0 - keep) * target);
        }
        g[3] = static_cast<float>(keep * g[3] + (1.0 - keep) * (fitted ? 1.0 : 0.0));
    }
    m_gainHas = true;
}

} // namespace insv
