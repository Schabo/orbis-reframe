// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SeamFade.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace insv {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

// Strip rows per degree of elevation.
double RowsPerDeg(const SeamFlowGrid& g) { return (g.rows - 1) / (2.0 * g.stripRad / kDeg); }

// Bilinear in a strip (x wraps round, y clamps); false where a tap has no
// sample (0).
bool StripSample(const std::vector<uint8_t>& s, int C, int R, double x, double y, double& out)
{
    y = std::clamp(y, 0.0, R - 1.0);
    const int y0 = std::min(static_cast<int>(y), R - 2);
    const double ay = y - y0;
    const double fx = std::floor(x);
    const double ax = x - fx;
    const int x0 = ((static_cast<int>(fx) % C) + C) % C;
    const int x1 = (x0 + 1) % C;
    const uint8_t* r0 = s.data() + static_cast<size_t>(y0) * C;
    const uint8_t* r1 = r0 + C;
    if (!r0[x0] || !r0[x1] || !r1[x0] || !r1[x1])
        return false;
    out = (r0[x0] * (1 - ax) + r0[x1] * ax) * (1 - ay) + (r1[x0] * (1 - ax) + r1[x1] * ax) * ay;
    return true;
}
} // namespace

int SeamFadeRows(const SeamFlowGrid& g)
{
    if (!g.valid())
        return 0;
    // (a hair over, so a whole number of rows from a float stripRad stays whole)
    const int reach = static_cast<int>(kSeamFadeNearDeg * RowsPerDeg(g) + 1e-4) / kSeamFadeRowStep;
    return 2 * reach + 1;
}

int SeamFadeRowY(const SeamFlowGrid& g, int r)
{
    const int n = SeamFadeRows(g);
    return (g.rows - 1) / 2 + (r - n / 2) * kSeamFadeRowStep;
}

void MeasureSeamFadeFrame(const SeamFlowStrips& raw, const SeamFlowField& field, SeamFadeFrame& out)
{
    const SeamFlowGrid& g = raw.grid;
    out.grid = g;
    const int C = g.cols, R = g.rows, N = SeamFadeRows(g);
    for (int i = 0; i < 2; ++i)
        out.mean[i].assign(static_cast<size_t>(N) * C, std::numeric_limits<float>::quiet_NaN());
    if (N == 0 || field.empty() || !(field.grid == g))
        return;
    // The rows the patches cover, each lens warped by its share of the flow
    // at the pixel (FisheyeStitcher::seamFlowRay on the strip's grid); a
    // pixel counts where both lenses have it.
    const int hy = kSeamFadePatchH / 2, hx = kSeamFadePatchW / 2;
    const int yLo = std::max(0, SeamFadeRowY(g, 0) - hy);
    const int yHi = std::min(R - 1, SeamFadeRowY(g, N - 1) + hy);
    const int H = yHi - yLo + 1;
    std::vector<float> v[2];
    std::vector<uint8_t> ok(static_cast<size_t>(H) * C, 0);
    for (int i = 0; i < 2; ++i)
        v[i].assign(static_cast<size_t>(H) * C, 0.0f);
    const double radX = 2.0 * kPi / C, radY = -2.0 * g.stripRad / (R - 1);
    for (int y = yLo; y <= yHi; ++y) {
        const double b = SeamFlowShare(g.eOf(y), g.reachRad);
        for (int x = 0; x < C; ++x) {
            float f[4];
            field.at(x, y, f);
            double s0, s1;
            if (!StripSample(raw.grey[0], C, R, x + b * f[0] / radX, y + b * f[1] / radY, s0) ||
                !StripSample(raw.grey[1], C, R, x + (1 - b) * f[2] / radX, y + (1 - b) * f[3] / radY, s1))
                continue;
            const size_t k = static_cast<size_t>(y - yLo) * C + x;
            v[0][k] = static_cast<float>(s0);
            v[1][k] = static_cast<float>(s1);
            ok[k] = 1;
        }
    }
    // Patch means round each centre.
    const int area = kSeamFadePatchW * kSeamFadePatchH;
    for (int r = 0; r < N; ++r) {
        const int yc = SeamFadeRowY(g, r);
        for (int x = 0; x < C; ++x) {
            double sum[2] = {0, 0};
            int n = 0;
            for (int dy = -hy; dy <= hy; ++dy) {
                const int y = yc + dy;
                if (y < yLo || y > yHi)
                    continue;
                const size_t row = static_cast<size_t>(y - yLo) * C;
                for (int dx = -hx; dx <= hx; ++dx) {
                    const size_t k = row + ((x + dx) % C + C) % C;
                    if (!ok[k])
                        continue;
                    sum[0] += v[0][k];
                    sum[1] += v[1][k];
                    ++n;
                }
            }
            if (2 * n < area)
                continue;
            for (int i = 0; i < 2; ++i)
                out.mean[i][static_cast<size_t>(r) * C + x] = static_cast<float>(sum[i] / n);
        }
    }
}

void SeamFadeShares(const SeamFadeFrame& f, const float* seamColour, std::vector<float>& share)
{
    const SeamFlowGrid& g = f.grid;
    const int C = g.cols, N = SeamFadeRows(g);
    share.assign(C, 0.0f);
    for (int x = 0; x < C; ++x) {
        // The colour blend's ratio at the column (seamColourLog on the seam,
        // where it is whole), as luma: each lens moved half of it.
        double half = 0.0;
        if (seamColour) {
            const double t = g.phiOf(x) / (2.0 * kPi) * kSeamDepthBins - 0.5;
            const double fl = std::floor(t);
            const int b0 = (static_cast<int>(fl) + kSeamDepthBins) % kSeamDepthBins;
            const int b1 = (b0 + 1) % kSeamDepthBins;
            const double a = t - fl;
            constexpr double kLuma[3] = {0.299, 0.587, 0.114};
            for (int c = 0; c < 3; ++c)
                half += 0.5 * kLuma[c] * (seamColour[b0 * 3 + c] * (1 - a) + seamColour[b1 * 3 + c] * a);
        }
        const double g0 = std::exp(-half), g1 = std::exp(half);
        double d = 0.0;
        for (int r = 0; r < N; ++r) {
            const float m0 = f.mean[0][static_cast<size_t>(r) * C + x];
            const float m1 = f.mean[1][static_cast<size_t>(r) * C + x];
            if (std::isnan(m0) || std::isnan(m1))
                continue;
            d = std::max(d, std::fabs(m0 * g0 - m1 * g1));
        }
        share[x] = static_cast<float>(std::clamp((d - kSeamFadeLo) / (kSeamFadeHi - kSeamFadeLo), 0.0, 1.0));
    }
}

void SeamFadeAlongSeam(const SeamFlowGrid& g, std::vector<float>& share)
{
    const int C = g.cols;
    if (C <= 0 || static_cast<int>(share.size()) != C)
        return;
    const double colsPerDeg = C / 360.0;
    const int spread = static_cast<int>(std::lround(kSeamFadeSpreadDeg * colsPerDeg));
    std::vector<float> m(C);
    for (int x = 0; x < C; ++x) {
        float v = 0.0f;
        for (int d = -spread; d <= spread; ++d)
            v = std::max(v, share[((x + d) % C + C) % C]);
        m[x] = v;
    }
    const double sigma = kSeamFadeSmoothDeg * colsPerDeg;
    const int rad = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<double> w(2 * rad + 1);
    double ws = 0.0;
    for (int d = -rad; d <= rad; ++d)
        ws += w[d + rad] = std::exp(-0.5 * d * d / (sigma * sigma));
    for (int x = 0; x < C; ++x) {
        double s = 0.0;
        for (int d = -rad; d <= rad; ++d)
            s += w[d + rad] * m[((x + d) % C + C) % C];
        share[x] = static_cast<float>(s / ws);
    }
}

void SeamFadeWindow::add(SeamFadeFrame f)
{
    if (!frames_.empty() && !(frames_.back().grid == f.grid))
        frames_.clear();
    frames_.push_back(std::move(f));
    while (static_cast<int>(frames_.size()) > kSeamFlowFrames)
        frames_.pop_front();
}

void SeamFadeWindow::solve(const float* seamColour, std::vector<float>& share) const
{
    share.clear();
    if (frames_.empty())
        return;
    const SeamFlowGrid& g = frames_.back().grid;
    share.assign(g.cols, 0.0f);
    std::vector<float> s;
    const int n = static_cast<int>(frames_.size());
    for (int k = 0; k < n; ++k) { // k frames before the newest
        SeamFadeShares(frames_[n - 1 - k], seamColour, s);
        const float w = 1.0f - static_cast<float>(k) / kSeamFlowFrames;
        for (int x = 0; x < g.cols; ++x)
            share[x] = std::max(share[x], w * s[x]);
    }
    SeamFadeAlongSeam(g, share);
}

} // namespace insv
