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
// there (the cut's disagreement, zsad / (texture + 4), below this), and every
// mean channel of both lenses lies between these levels: a dark channel's
// ratio is noise, a near-clipped one is cut off in one lens.
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

struct Cell {
    double L[3]; // log(lens 0 / lens 1), R G B
    double eDeg; // the row's elevation, towards lens 0
    double w;    // unclipped samples
    int row;
    int bin;
};

std::vector<Cell> usableCells(const SeamCutStats& s, double bandRad)
{
    std::vector<Cell> out;
    for (int b = 0; b < kSeamDepthBins; ++b)
        for (int r = 0; r < kSeamCutRows; ++r) {
            const SeamCutStats::Cell& c = s.cell[b][r];
            if (c.n < kCutSamples / 2 || c.nColour < kCutSamples / 2 ||
                c.zsad / (c.texture + 4.0) > kMaxDisagreement)
                continue;
            bool ok = true;
            for (int i = 0; i < 2; ++i)
                for (int k = 0; k < 3; ++k)
                    ok = ok && c.mean[i][k] > kMinLevel && c.mean[i][k] < kMaxLevel;
            if (!ok)
                continue;
            Cell x;
            for (int k = 0; k < 3; ++k)
                x.L[k] = std::log(static_cast<double>(c.mean[0][k]) / c.mean[1][k]);
            x.eDeg = SeamCutRowE(r, bandRad) / kDeg;
            x.w = c.nColour;
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
// row, null = none) and the share of its cells far off it.
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
            v[k].emplace_back(d[i * 3 + k], cells[i].w);
        }
    }
    for (int k = 0; k < 3; ++k)
        median[k] = weightedMedian(v[k]);
    int far = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        double dev = 0;
        for (int k = 0; k < 3; ++k)
            dev = std::max(dev, std::fabs(d[i * 3 + k] - median[k]));
        far += dev > kFarLog ? 1 : 0;
    }
    return cells.empty() ? 1.0 : static_cast<double>(far) / cells.size();
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
                rows[c.row][c.bin * kSectors / kSeamDepthBins][k].emplace_back(c.L[k], c.w);
            ++out.cells;
        }
    }

    // Per row and stretch of the seam the median, per row the median over
    // the stretches -- a difference on one side of the seam only (a low
    // sun's veiling light in one lens, in frames the flare test lets
    // through) doesn't move it -- then per channel the line through the
    // rows, each row weighted by its cells.
    double sw = 0, se = 0, see = 0, sl[3] = {}, sel[3] = {};
    int usedRows = 0;
    for (int r = 0; r < kSeamCutRows; ++r) {
        std::vector<std::pair<double, double>> sectors[3];
        double n = 0;
        for (int q = 0; q < kSectors; ++q) {
            if (static_cast<int>(rows[r][q][0].size()) < kMinSectorCells)
                continue;
            n += static_cast<double>(rows[r][q][0].size());
            for (int k = 0; k < 3; ++k)
                sectors[k].emplace_back(weightedMedian(rows[r][q][k]), 1.0);
        }
        if (static_cast<int>(sectors[0].size()) < kMinRowSectors)
            continue;
        ++usedRows;
        const double e = SeamCutRowE(r, bandRad) / kDeg;
        sw += n, se += n * e, see += n * e * e;
        for (int k = 0; k < 3; ++k) {
            const double m = weightedMedian(sectors[k]);
            sl[k] += n * m, sel[k] += n * e * m;
        }
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
    const double det = sw * see - se * se;
    for (int k = 0; k < 3; ++k) {
        const double slope = det > 1e-9 ? (sw * sel[k] - se * sl[k]) / det : 0.0;
        out.colour.slope[k] = slope;
        out.colour.logRatio[k] = std::clamp((sl[k] - slope * se) / sw, -kLensColourMaxLog,
                                            kLensColourMaxLog);
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
                      const double residual[3], float* value, float* weight)
{
    const std::vector<Cell> cells = usableCells(stats, bandRad);
    std::vector<std::pair<double, double>> v[kSeamDepthBins][3];
    for (const Cell& x : cells) {
        double m[3];
        LensColourLog(c, x.eDeg * kDeg, bandRad, residual, m);
        for (int k = 0; k < 3; ++k)
            v[x.bin][k].emplace_back(x.L[k] - m[k], x.w);
    }
    for (int b = 0; b < kSeamDepthBins; ++b) {
        double w = 0;
        for (const auto& [d, wt] : v[b][0])
            w += wt;
        weight[b] = static_cast<float>(w);
        for (int k = 0; k < 3; ++k)
            value[b * 3 + k] = static_cast<float>(weightedMedian(v[b][k]));
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

} // namespace insv
