// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SeamColourBlend.h"

#include "LensColour.h"
#include "SeamCut.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace insv {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;
constexpr int B = kSeamDepthBins;

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}
} // namespace

double SeamColourFade(double eRad)
{
    const double e = std::fabs(eRad) / kDeg;
    if (e <= kSeamColourFullDeg)
        return 1.0;
    if (e >= kSeamColourEndDeg)
        return 0.0;
    return 0.5 * (1.0 + std::cos(kPi * (e - kSeamColourFullDeg) /
                                 (kSeamColourEndDeg - kSeamColourFullDeg)));
}

void MeasureSeamColourBins(const SeamCutStats& stats, double bandRad, const SeamColourTuning& T,
                           SeamColourBins& out)
{
    float* v = &out.value[0][0];
    float* w = &out.weight[0][0];
    std::vector<float> spread(B * 3);
    // No lens colour or residual to measure against: the whole ratio, every
    // row across the band alike.
    LensColourPerBin(stats, bandRad, LensColour{}, nullptr, nullptr, nullptr, v, w, spread.data());
    // A bin stands on its own when it has enough unclipped samples and its
    // cells agree about as well as the frame's typical bin's do; flare or
    // something close crossing the seam spreads them.
    for (int k = 0; k < 3; ++k) {
        std::vector<double> s;
        for (int b = 0; b < B; ++b)
            if (w[b * 3 + k] >= T.minSamples)
                s.push_back(spread[b * 3 + k]);
        const double limit = std::max(T.spreadFactor * median(s), T.spreadFloor);
        for (int b = 0; b < B; ++b)
            if (w[b * 3 + k] < T.minSamples || spread[b * 3 + k] > limit)
                w[b * 3 + k] = 0.0f;
    }
}

bool SolveSeamColour(const double* wv, const double* w, const SeamColourTuning& T, float* table)
{
    bool any = false;
    const double sigma = std::max(T.smoothDeg * kDeg / (2.0 * kPi / B), 0.5);
    const int reach = static_cast<int>(std::ceil(2.5 * sigma));
    for (int k = 0; k < 3; ++k) {
        std::vector<double> sm(B, 0.0);
        std::vector<bool> has(B, false);
        for (int b = 0; b < B; ++b) {
            double sw = 0, s = 0;
            for (int d = -reach; d <= reach; ++d) {
                const int j = (b + d + B) % B;
                const double g = std::exp(-0.5 * d * d / (sigma * sigma));
                sw += g * w[j * 3 + k];
                s += g * wv[j * 3 + k];
            }
            if (sw > 0) {
                sm[b] = s / sw;
                has[b] = true;
                any = true;
            }
        }
        // Stretches the smoothing didn't reach: between the nearest bins it
        // did either side, round the seam.
        if (std::find(has.begin(), has.end(), true) != has.end()) {
            for (int b = 0; b < B; ++b) {
                if (has[b])
                    continue;
                int lo = b, hi = b, dl = 0, dh = 0;
                while (!has[lo]) {
                    lo = (lo - 1 + B) % B;
                    ++dl;
                }
                while (!has[hi]) {
                    hi = (hi + 1) % B;
                    ++dh;
                }
                sm[b] = (sm[lo] * dh + sm[hi] * dl) / (dl + dh);
            }
        }
        for (int b = 0; b < B; ++b)
            table[b * 3 + k] =
                static_cast<float>(std::clamp(sm[b], -kSeamColourMaxLog, kSeamColourMaxLog));
    }
    return any;
}

bool MeasureSeamColour(const SeamCutStats& stats, double bandRad, const SeamColourTuning& T,
                       float* table)
{
    SeamColourBins bins;
    MeasureSeamColourBins(stats, bandRad, T, bins);
    std::vector<double> wv(B * 3), w(B * 3);
    for (int i = 0; i < B * 3; ++i) {
        w[i] = (&bins.weight[0][0])[i];
        wv[i] = w[i] * (&bins.value[0][0])[i];
    }
    return SolveSeamColour(wv.data(), w.data(), T, table);
}

void SeamColourWindow::add(int64_t ptsMs, const SeamCutStats& stats, double bandRad)
{
    if (!m_frames.empty() && ptsMs <= m_frames.back().pts)
        m_frames.clear();
    m_frames.push_back({ptsMs, {}});
    MeasureSeamColourBins(stats, bandRad, m_tuning, m_frames.back().bins);
}

bool SeamColourWindow::solve(int64_t ptsMs, float* table) const
{
    std::vector<double> wv(B * 3, 0.0), w(B * 3, 0.0);
    for (const Frame& f : m_frames) {
        const double dt = static_cast<double>(ptsMs - f.pts);
        if (dt < 0 || dt > kSeamColourWindowMs)
            continue;
        const double a = 1.0 - dt / (kSeamColourWindowMs + 50.0);
        for (int i = 0; i < B * 3; ++i) {
            const double wt = a * (&f.bins.weight[0][0])[i];
            w[i] += wt;
            wv[i] += wt * (&f.bins.value[0][0])[i];
        }
    }
    return SolveSeamColour(wv.data(), w.data(), m_tuning, table);
}

void SeamColourWindow::dropBefore(int64_t ptsMs)
{
    std::size_t n = 0;
    while (n < m_frames.size() && m_frames[n].pts < ptsMs)
        ++n;
    m_frames.erase(m_frames.begin(), m_frames.begin() + static_cast<std::ptrdiff_t>(n));
}

} // namespace insv
