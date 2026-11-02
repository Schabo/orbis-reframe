// SPDX-License-Identifier: MIT OR Apache-2.0
#include "LensColour.h"

#include "SeamCut.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>

namespace insv {

namespace {
constexpr double kDeg = std::numbers::pi / 180.0;
constexpr int kCutSamples = kSeamCutPatchAz * kSeamCutPatchEl;
// A cell counts when both lenses see most of its patch, see the same thing
// there (the cut's disagreement, zsad / (texture + 4), below this), and no
// mean channel of either lens is below kMinLevel (a dark channel's ratio is
// noise); a channel at or above kMaxLevel in either lens (cut off in one) is
// left out of that cell on its own.
constexpr double kMaxDisagreement = 0.5;
constexpr double kMinLevel = 16.0;
constexpr double kMaxLevel = 235.0;
// A frame with fewer usable cells says nothing.
constexpr int kMinCells = 40;
// A cell is far off its frame when a channel's log ratio is this far from the
// frame's median; a frame is flare when the share of far cells is above the
// clip's typical share by kFlareMargin, and never below kFlareFloor (on the
// A1 and X5 ordinary frames have 10-30 %, flare frames 35-65 %).
constexpr double kFarLog = 0.1;
constexpr double kFlareFloor = 0.33;
constexpr double kFlareMargin = 0.15;
// The fit: the seam circle in kSectors stretches; per row across the band and
// stretch the median over the clip's cells there (kMinSectorCells at least),
// per row the median over at least kMinRowSectors stretches; at least
// kMinRows rows and kMinFrames frames.
constexpr int kSectors = 8;
constexpr int kMinSectorCells = 10;
constexpr int kMinRowSectors = 3;
constexpr int kMinRows = 3;
constexpr int kMinFrames = 3;
// Veiling light (LensVeilPerBin): per bin from its cells pooled with
// kLensVeilPoolBins bins either side, kLensVeilMinCells of them at least,
// then the median of those over kLensVeilReachBins either side. The darker
// and the brighter third kLensVeilMinSpan apart (linear) at least, and the
// darker third no further from black than that, or a gain and an offset
// can't be told apart -- then only a difference beyond any gain the colour
// match allows (kLensColourLocalMaxLog) counts as light added. None below
// kLensVeilThreshold (linear, green), in full from twice that.
constexpr int kLensVeilPoolBins = 1;
constexpr int kLensVeilReachBins = 3;
constexpr int kLensVeilMinCells = 9;
constexpr double kLensVeilMinSpan = 0.05;
constexpr double kLensVeilThreshold = 0.02;
// Each channel's veil at least this share of green's, the same way.
constexpr double kLensVeilMinShare = 0.25;

struct Cell {
    double L[3]; // log(lens 0 / lens 1), R G B, where w > 0
    double w[3]; // the channel's unclipped samples; 0 = that channel isn't usable
    double eDeg; // the row's elevation, towards lens 0
    int row;
    int bin;
};

std::vector<Cell> usableCells(const SeamCutStats& s, double bandRad)
{
    std::vector<Cell> out;
    for (int b = 0; b < kSeamDepthBins; ++b)
        for (int r = 0; r < kSeamCutRows; ++r) {
            const SeamCutStats::Cell& c = s.cell[b][r];
            if (c.n < kCutSamples / 2 || c.zsad / (c.texture + 4.0) > kMaxDisagreement)
                continue;
            // A dark channel in either lens says nothing in any (noise); a
            // clipped one only leaves itself out.
            bool dark = false;
            for (int i = 0; i < 2; ++i)
                for (int k = 0; k < 3; ++k)
                    dark = dark || (c.nColour[k] >= kCutSamples / 2 && c.mean[i][k] <= kMinLevel);
            if (dark)
                continue;
            Cell x;
            bool any = false;
            for (int k = 0; k < 3; ++k) {
                const bool ok = c.nColour[k] >= kCutSamples / 2 && c.mean[0][k] < kMaxLevel &&
                                c.mean[1][k] < kMaxLevel;
                x.w[k] = ok ? c.nColour[k] : 0.0;
                x.L[k] = ok ? std::log(static_cast<double>(c.mean[0][k]) / c.mean[1][k]) : 0.0;
                any = any || ok;
            }
            if (!any)
                continue;
            x.eDeg = SeamCutRowE(r, bandRad) / kDeg;
            x.row = r;
            x.bin = b;
            out.push_back(x);
        }
    return out;
}

double weightedMedian(std::vector<std::pair<double, double>>& v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    double half = 0;
    for (const auto& [x, w] : v)
        half += w;
    half *= 0.5;
    for (const auto& [x, w] : v)
        if ((half -= w) <= 0)
            return x;
    return v.back().first;
}

// The frame's median log ratio per channel (cells' L less `model` at their
// row, null = none) and the share of its cells far off it in any channel
// they have.
double frameSpread(const std::vector<Cell>& cells, const LensColour* model, double bandRad,
                   double median[3])
{
    std::vector<std::pair<double, double>> v[3];
    std::vector<double> d(cells.size() * 3);
    for (size_t i = 0; i < cells.size(); ++i) {
        double m[3] = {0, 0, 0};
        if (model)
            LensColourLog(*model, cells[i].eDeg * kDeg, bandRad, nullptr, m);
        for (int k = 0; k < 3; ++k) {
            d[i * 3 + k] = cells[i].L[k] - m[k];
            if (cells[i].w[k] > 0)
                v[k].emplace_back(d[i * 3 + k], cells[i].w[k]);
        }
    }
    for (int k = 0; k < 3; ++k)
        median[k] = weightedMedian(v[k]);
    int far = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        double dev = 0;
        for (int k = 0; k < 3; ++k)
            if (cells[i].w[k] > 0)
                dev = std::max(dev, std::fabs(d[i * 3 + k] - median[k]));
        far += dev > kFarLog ? 1 : 0;
    }
    return cells.empty() ? 1.0 : static_cast<double>(far) / cells.size();
}

double linear(double v)
{
    return std::pow(std::clamp(v / 255.0, 0.0, 1.0), kLensVeilGamma);
}

double median(std::vector<double>& v)
{
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}
} // namespace

void LensColourLog(const LensColour& c, double eRad, double bandRad, const double* residual,
                   double out[3])
{
    const double e = std::clamp(eRad, -bandRad, bandRad) / kDeg;
    for (int k = 0; k < 3; ++k)
        out[k] = std::clamp(c.logRatio[k] + c.slope[k] * e + (residual ? residual[k] : 0.0),
                            -kLensColourMaxLog, kLensColourMaxLog);
}

double LensColourGain(double lg, double v)
{
    const double t =
        std::clamp((v - kLensColourKneeLo) / (kLensColourKneeHi - kLensColourKneeLo), 0.0, 1.0);
    return std::exp(lg * (1.0 - t * t * (3.0 - 2.0 * t)));
}

LensColourFit FitLensColour(const std::vector<const SeamCutStats*>& frames, double bandRad)
{
    LensColourFit out;
    std::vector<std::vector<Cell>> cells;
    std::vector<double> far;
    for (const SeamCutStats* s : frames) {
        if (!s)
            continue;
        std::vector<Cell> c = usableCells(*s, bandRad);
        if (static_cast<int>(c.size()) < kMinCells)
            continue;
        double m[3];
        far.push_back(frameSpread(c, nullptr, bandRad, m));
        cells.push_back(std::move(c));
    }
    std::vector<double> sorted = far;
    std::sort(sorted.begin(), sorted.end());
    const double spread = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];
    const double flareAt = std::max(kFlareFloor, spread + kFlareMargin);

    std::vector<std::pair<double, double>> rows[kSeamCutRows][kSectors][3];
    for (size_t f = 0; f < cells.size(); ++f) {
        if (far[f] > flareAt) {
            ++out.flareFrames;
            continue;
        }
        ++out.frames;
        for (const Cell& c : cells[f]) {
            for (int k = 0; k < 3; ++k)
                if (c.w[k] > 0)
                    rows[c.row][c.bin * kSectors / kSeamDepthBins][k].emplace_back(c.L[k], c.w[k]);
            ++out.cells;
        }
    }

    // Per channel, per row and stretch of the seam the median, per row the
    // median over the stretches -- a difference on one side of the seam
    // only (a low sun's veiling light in one lens, in frames the flare test
    // lets through) doesn't move it -- then the line through the rows, each
    // row weighted by its cells.
    int usedRows = kSeamCutRows;
    double fitted[3][2] = {};
    for (int k = 0; k < 3; ++k) {
        double sw = 0, se = 0, see = 0, sl = 0, sel = 0;
        int used = 0;
        for (int r = 0; r < kSeamCutRows; ++r) {
            std::vector<std::pair<double, double>> sectors;
            double n = 0;
            for (int q = 0; q < kSectors; ++q) {
                if (static_cast<int>(rows[r][q][k].size()) < kMinSectorCells)
                    continue;
                n += static_cast<double>(rows[r][q][k].size());
                sectors.emplace_back(weightedMedian(rows[r][q][k]), 1.0);
            }
            if (static_cast<int>(sectors.size()) < kMinRowSectors)
                continue;
            ++used;
            const double e = SeamCutRowE(r, bandRad) / kDeg;
            const double m = weightedMedian(sectors);
            sw += n, se += n * e, see += n * e * e, sl += n * m, sel += n * e * m;
        }
        usedRows = std::min(usedRows, used);
        const double det = sw * see - se * se;
        fitted[k][1] = det > 1e-9 ? (sw * sel - se * sl) / det : 0.0;
        fitted[k][0] = sw > 0 ? (sl - fitted[k][1] * se) / sw : 0.0;
    }
    char buf[256];
    if (out.frames < kMinFrames || usedRows < kMinRows) {
        std::snprintf(buf, sizeof buf,
                      "too little to go on: %d usable frames (%d with flare), %d rows of seam "
                      "cells",
                      out.frames, out.flareFrames, usedRows);
        out.message = buf;
        return out;
    }
    for (int k = 0; k < 3; ++k) {
        out.colour.slope[k] = fitted[k][1];
        out.colour.logRatio[k] = std::clamp(fitted[k][0], -kLensColourMaxLog, kLensColourMaxLog);
    }
    out.colour.spread = spread;
    out.ok = true;
    const LensColour& c = out.colour;
    std::snprintf(buf, sizeof buf,
                  "lens 0 / 1 R %+.1f %% G %+.1f %% B %+.1f %%, across the band %+.2f / %+.2f / "
                  "%+.2f %%/deg; %d frames (%d with flare left out), %d cells, spread %.0f %%",
                  100.0 * std::expm1(c.logRatio[0]), 100.0 * std::expm1(c.logRatio[1]),
                  100.0 * std::expm1(c.logRatio[2]), 100.0 * c.slope[0], 100.0 * c.slope[1],
                  100.0 * c.slope[2], out.frames, out.flareFrames, out.cells, 100.0 * spread);
    out.message = buf;
    return out;
}

void LensColourPerBin(const SeamCutStats& stats, double bandRad, const LensColour& c,
                      const double residual[3], const float* meetRad, const float* reachRad,
                      float* value, float* weight, float* spread)
{
    // Per bin, each cell weighted by how near it lies to where the lenses
    // meet (meetRad, reachRad: a Gaussian across the band; null = all rows
    // alike). From the cells all three channels are usable in, so every
    // channel is measured on the same content (a channel left out of some
    // cells, near clipping, would be matched on others and shift the hue),
    // unless most of the weight is in cells with a channel clipped (a sky
    // clipped in blue): then each channel from the cells it has.
    const std::vector<Cell> cells = usableCells(stats, bandRad);
    std::vector<std::pair<double, double>> all[kSeamDepthBins][3], some[kSeamDepthBins][3];
    double wAll[kSeamDepthBins] = {}, wSome[kSeamDepthBins] = {};
    for (const Cell& x : cells) {
        double m[3];
        LensColourLog(c, x.eDeg * kDeg, bandRad, residual, m);
        double near = 1.0;
        if (meetRad) {
            const double d = (x.eDeg * kDeg - meetRad[x.bin]) / reachRad[x.bin];
            near = std::exp(-0.5 * d * d);
        }
        const bool full = x.w[0] > 0 && x.w[1] > 0 && x.w[2] > 0;
        (full ? wAll : wSome)[x.bin] += near * std::max({x.w[0], x.w[1], x.w[2]});
        for (int k = 0; k < 3; ++k)
            if (x.w[k] > 0)
                (full ? all : some)[x.bin][k].emplace_back(x.L[k] - m[k], near * x.w[k]);
    }
    for (int b = 0; b < kSeamDepthBins; ++b) {
        const bool useAll = wAll[b] > 0 && wAll[b] >= wSome[b];
        for (int k = 0; k < 3; ++k) {
            auto& v = useAll ? all[b][k] : some[b][k];
            double w = 0;
            for (const auto& [d, wt] : v)
                w += wt;
            weight[b * 3 + k] = static_cast<float>(w);
            const double med = weightedMedian(v);
            value[b * 3 + k] = static_cast<float>(med);
            if (spread) {
                for (auto& [d, wt] : v)
                    d = std::fabs(d - med);
                spread[b * 3 + k] = v.size() >= 2 ? static_cast<float>(weightedMedian(v)) : 0.0f;
            }
        }
    }
}

bool LensColourResidual(const SeamCutStats& stats, double bandRad, const LensColour& c,
                        double out[3])
{
    const std::vector<Cell> cells = usableCells(stats, bandRad);
    if (static_cast<int>(cells.size()) < kMinCells)
        return false;
    double m[3];
    if (frameSpread(cells, &c, bandRad, m) > std::max(kFlareFloor, c.spread + kFlareMargin))
        return false;
    for (int k = 0; k < 3; ++k)
        out[k] = std::clamp(m[k], -kLensColourResidualMaxLog, kLensColourResidualMaxLog);
    return true;
}

double LensVeilRemove(double v, double veil)
{
    if (veil <= 0.0)
        return v;
    const double t =
        std::clamp((v - kLensColourKneeLo) / (kLensColourKneeHi - kLensColourKneeLo), 0.0, 1.0);
    const double lin = linear(v);
    const double out = std::max(lin - veil * (1.0 - t * t * (3.0 - 2.0 * t)), lin * kLensVeilKeep);
    return 255.0 * std::pow(out, 1.0 / kLensVeilGamma);
}

void LensVeilPerBin(const SeamCutStats& stats, float* veil)
{
    constexpr int B = kSeamDepthBins;
    // Per cell the linear means of both lenses; NaN where a channel isn't
    // usable (clipped, or a cell the lenses disagree on).
    struct V {
        double lin[2][3];
        double level;
    };
    std::vector<std::vector<V>> perBin(B);
    for (int b = 0; b < B; ++b)
        for (int r = 0; r < kSeamCutRows; ++r) {
            const SeamCutStats::Cell& c = stats.cell[b][r];
            if (c.n < kCutSamples / 2 || c.zsad / (c.texture + 4.0) > kMaxDisagreement)
                continue;
            V x;
            for (int k = 0; k < 3; ++k) {
                const bool ok = c.nColour[k] >= kCutSamples / 2 && c.mean[0][k] < kMaxLevel &&
                                c.mean[1][k] < kMaxLevel;
                for (int i = 0; i < 2; ++i)
                    x.lin[i][k] = ok ? linear(c.mean[i][k]) : NAN;
            }
            if (std::isnan(x.lin[0][1]))
                continue; // green ranks the cells
            x.level = 0.5 * (x.lin[0][1] + x.lin[1][1]);
            perBin[b].push_back(x);
        }

    // Each bin's own estimate, from its cells and its neighbours'
    // (kLensVeilPoolBins either side); NaN where it can't tell.
    std::vector<V> cells;
    std::vector<double> est(B * 3, NAN);
    for (int b = 0; b < B; ++b) {
        cells.clear();
        for (int d = -kLensVeilPoolBins; d <= kLensVeilPoolBins; ++d) {
            const std::vector<V>& q = perBin[(b + d + B) % B];
            cells.insert(cells.end(), q.begin(), q.end());
        }
        if (static_cast<int>(cells.size()) < kLensVeilMinCells)
            continue;
        std::sort(cells.begin(), cells.end(),
                  [](const V& a, const V& c) { return a.level < c.level; });
        const size_t third = cells.size() / 3;
        // Per channel through the darker and the brighter third: lens 1 =
        // gain * lens 0 + offset. The offset is light one lens adds; the
        // gain is the lenses' colour, left to LensColour.
        double v[3], dark[3];
        bool known[3];
        for (int k = 0; k < 3; ++k) {
            std::vector<double> x[2][2]; // [dark, bright][lens]
            for (size_t i = 0; i < cells.size(); ++i) {
                const int g = i < third ? 0 : i >= cells.size() - third ? 1 : -1;
                if (g < 0 || std::isnan(cells[i].lin[0][k]))
                    continue;
                x[g][0].push_back(cells[i].lin[0][k]);
                x[g][1].push_back(cells[i].lin[1][k]);
            }
            known[k] = false;
            dark[k] = 0.0;
            if (x[0][0].size() < 3)
                continue;
            const double x0 = median(x[0][0]), y0 = median(x[0][1]);
            dark[k] = y0 - x0;
            double x1 = 0, y1 = 0;
            if (x[1][0].size() >= 3)
                x1 = median(x[1][0]), y1 = median(x[1][1]);
            // The line is followed down to no light at most as far again as
            // the two thirds lie apart: further, a little noise in the
            // gain makes a large offset.
            const double span = x1 - x0;
            const double gain = span >= kLensVeilMinSpan && x0 <= span ? (y1 - y0) / span : 0.0;
            if (gain >= 0.5 && gain <= 2.0) {
                // Lens 1 adds the offset; or lens 0 adds -offset / gain.
                const double offset = y0 - gain * x0;
                v[k] = offset > 0 ? -offset : -offset / gain;
                known[k] = true;
            } else if (std::fabs(std::log(std::pow(std::max(y0, 1e-6) / std::max(x0, 1e-6),
                                                   1.0 / kLensVeilGamma))) >
                       kLensColourLocalMaxLog) {
                // No gain and offset to be had (the cells all about as
                // bright), but the darker ones further apart than any gain
                // the colour match allows: light added.
                v[k] = -dark[k];
                known[k] = true;
            }
        }
        if (!known[1])
            continue;
        // Green decides whether there is any, and which lens has it; a
        // channel without its own fit takes green's, scaled as the darker
        // cells differ. Light the sun scatters lifts every channel: where
        // one goes the other way or hardly at all, it isn't that, and a
        // veil taken out of one channel only would tint the lens.
        const double g = v[1];
        const double s =
            std::clamp((std::fabs(g) - kLensVeilThreshold) / kLensVeilThreshold, 0.0, 1.0);
        double x[3];
        bool lifts = true;
        for (int k = 0; k < 3; ++k) {
            x[k] = known[k] ? v[k] : dark[1] * dark[k] > 0 ? g * dark[k] / dark[1] : 0.0;
            lifts = lifts && x[k] * g > 0 && std::fabs(x[k]) >= kLensVeilMinShare * std::fabs(g);
        }
        for (int k = 0; k < 3; ++k)
            est[b * 3 + k] = lifts ? x[k] * s : 0.0;
    }

    // The median of the estimates over kLensVeilReachBins either side: one
    // stretch's odd cells don't make a veil, nor take one away.
    std::vector<double> w;
    for (int b = 0; b < B; ++b)
        for (int k = 0; k < 3; ++k) {
            w.clear();
            for (int d = -kLensVeilReachBins; d <= kLensVeilReachBins; ++d) {
                const double e = est[((b + d + B) % B) * 3 + k];
                if (!std::isnan(e))
                    w.push_back(e);
            }
            veil[b * 3 + k] = w.size() < 3 ? 0.0f
                                           : static_cast<float>(std::clamp(median(w), -kLensVeilMax,
                                                                           kLensVeilMax));
        }
}



void RemoveLensVeil(SeamCutStats& stats, const float* veil)
{
    for (int b = 0; b < kSeamDepthBins; ++b)
        for (int k = 0; k < 3; ++k) {
            const double v = veil[b * 3 + k];
            if (v == 0.0)
                continue;
            const int i = v > 0 ? 0 : 1;
            for (int r = 0; r < kSeamCutRows; ++r) {
                float& m = stats.cell[b][r].mean[i][k];
                m = static_cast<float>(LensVeilRemove(m, std::fabs(v)));
            }
        }
}

} // namespace insv
