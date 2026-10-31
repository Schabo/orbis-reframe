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
            int n = 0, nc[3] = {0, 0, 0};
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
                for (int k = 0; k < 3; ++k)
                    if (std::max(c0[k], c1[k]) <= kSeamCutClip) {
                        m[0][k] += c0[k], m[1][k] += c1[k];
                        ++nc[k];
                    }
            }
            SeamCutStats::Cell& cell = out.cell[b][r];
            cell = SeamCutStats::Cell{};
            cell.n = static_cast<float>(n);
            for (int k = 0; k < 3; ++k) {
                cell.nColour[k] = static_cast<float>(nc[k]);
                for (int i = 0; i < 2 && nc[k] > 0; ++i)
                    cell.mean[i][k] = static_cast<float>(m[i][k] / nc[k]);
            }
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

void SeamCutDisagreement(const SeamCutStats& stats, const SeamCutTuning& T, float* dis)
{
    constexpr int B = kSeamDepthBins, R = kSeamCutRows;
    for (int b = 0; b < B; ++b)
        for (int r = 0; r < R; ++r) {
            const SeamCutStats::Cell& c = stats.cell[b][r];
            dis[static_cast<size_t>(b) * R + r] = static_cast<float>(
                c.n >= kCutSamples / 2 ? std::min(c.zsad / (c.texture + T.texFloor), kNoCell) : kNoCell);
        }
}

void SolveSeamCut(const float* dis, double bandRad, const SeamCutTuning& T, int* row, float* width)
{
    constexpr int B = kSeamDepthBins, R = kSeamCutRows;
    // The cut: one row per bin, by a circular DP along the azimuth (three
    // laps, the middle one kept, as SolveSeamDepth).
    double rowDeg[R];
    for (int r = 0; r < R; ++r)
        rowDeg[r] = SeamCutRowE(r, bandRad) / kDeg;
    auto unary = [&](int b, int r) {
        return dis[static_cast<size_t>(b) * R + r] + T.offCentre * std::fabs(rowDeg[r]);
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
    // along the azimuth so a wide stretch doesn't start abruptly.
    std::vector<float> at(B);
    for (int b = 0; b < B; ++b) {
        row[b] = label[B + b];
        at[b] = dis[static_cast<size_t>(b) * R + row[b]];
    }
    const int reach = static_cast<int>(std::ceil(2.0 * T.widthSigma));
    for (int b = 0; b < B; ++b) {
        double a = 0, w = 0;
        for (int d = -reach; d <= reach; ++d) {
            const double ww = T.widthSigma > 0 ? std::exp(-0.5 * d * d / (T.widthSigma * T.widthSigma))
                                               : (d == 0 ? 1.0 : 0.0);
            a += ww * smoothstep(T.agreeLo, T.agreeHi, at[(b + d + B) % B]);
            w += ww;
        }
        width[b] = static_cast<float>(a / w);
    }
}

void SeamCutMeet(const int* row, const float* width, double bandRad, float* meetRad,
                 float* reachRad)
{
    constexpr int B = kSeamDepthBins, R = kSeamCutRows;
    // The fade's centre moves to the seam circle as it widens, so it stays
    // inside the band.
    const double narrow = std::min(kSeamCutFadeDeg * kDeg, bandRad);
    const double rowStep = 2.0 * SeamCutRowMax(bandRad) / (R - 1);
    for (int b = 0; b < B; ++b) {
        const double s = width[b];
        meetRad[b] = static_cast<float>(SeamCutRowE(row[b], bandRad) * (1.0 - s));
        reachRad[b] = static_cast<float>(std::max(narrow + (bandRad - narrow) * s, rowStep));
    }
}

bool SeamFrameResidual(const SeamCutStats& stats, double bandRad, const LensColour& c,
                       double out[3])
{
    double r[3];
    if (!LensColourResidual(stats, bandRad, c, r))
        return false;
    std::vector<float> veil(kSeamDepthBins * 3);
    LensVeilPerBin(stats, veil.data());
    auto clean = std::make_unique<SeamCutStats>(stats);
    RemoveLensVeil(*clean, veil.data());
    if (!LensColourResidual(*clean, bandRad, c, r))
        return false;
    for (int k = 0; k < 3; ++k)
        out[k] = r[k];
    return true;
}

void SeamLocalCorrection(const SeamCutStats& clean, double bandRad, const LensColour& c,
                         const double residual[3], const float* meetRad, const float* reachRad,
                         const SeamCutTuning& T, float* local)
{
    constexpr int B = kSeamDepthBins;
    std::vector<float> v(B * 3), w(B * 3);
    LensColourPerBin(clean, bandRad, c, residual, meetRad, reachRad, v.data(), w.data());
    const double sigma = std::max(T.localSigma, 0.5);
    const int reach = static_cast<int>(std::ceil(2.5 * sigma));
    for (int b = 0; b < B; ++b)
        for (int k = 0; k < 3; ++k) {
            double sw = 0, s = 0;
            for (int d = -reach; d <= reach; ++d) {
                const int j = (b + d + B) % B;
                const double g = std::exp(-0.5 * d * d / (sigma * sigma)) * w[j * 3 + k];
                sw += g;
                s += g * v[j * 3 + k];
            }
            double own = 0;
            for (int d = -1; d <= 1; ++d)
                own += w[((b + d + B) % B) * 3 + k];
            const double conf = std::min(1.0, own / T.localMinSamples);
            local[b * 3 + k] = static_cast<float>(
                sw > 0 ? std::clamp(conf * s / sw, -kLensColourLocalMaxLog, kLensColourLocalMaxLog)
                       : 0.0);
        }
}

void SeamCutBlend(const int* row, const float* width, double bandRad, SeamBlend& blend)
{
    const double narrow = std::min(kSeamCutFadeDeg * kDeg, bandRad);
    for (int b = 0; b < kSeamDepthBins; ++b) {
        const double s = std::max(static_cast<double>(width[b]),
                                  std::min(1.0, std::fabs(blend.veil[b][1]) / kLensVeilWide));
        blend.cut[b] = static_cast<float>(SeamCutRowE(row[b], bandRad) * (1.0 - s));
        blend.fade[b] = static_cast<float>(narrow + (bandRad - narrow) * s);
    }
}

} // namespace insv
