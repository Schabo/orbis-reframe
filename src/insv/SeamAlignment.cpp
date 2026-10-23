// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SeamAlignment.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <thread>

namespace insv {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

// Strip around the seam circle in lens 0's frame: rows are the angle off
// lens 0's axis, kStripHalf cells either side of 90 deg; columns the azimuth
// around it. A cell is ~0.8 px of the half-res lens image at the seam.
constexpr double kCellDeg = 0.1;
constexpr int kStripHalf = 40; // +-4 deg
constexpr int kRows = 2 * kStripHalf + 1;
constexpr int kCols = 3600; // 360 deg

// Blocks: kBlockCols columns wide, matched over the middle +-kCoreHalf rows;
// lens 1's block is searched over +-search cells both ways.
constexpr int kBlockCols = 50; // 5 deg
constexpr int kBlocks = kCols / kBlockCols;
constexpr int kCoreHalf = 25; // +-2.5 deg
constexpr int kSearchFirst = 10; // +-1 deg from no correction
constexpr int kSearchLater = 5;  // +-0.5 deg once the fit is close
constexpr int kRounds = 3;

// A block is used when both lenses show texture (grey standard deviation, 0..255)
// and the best match is a clear peak.
constexpr double kMinTexture = 4.0;
constexpr double kMinPeak = 0.75;
constexpr int kMinBlocks = 12;

// The six numbers fitted, in units that move the seam by similar amounts:
// lens-1 yaw / pitch / roll in degrees, its principal point in thousandths
// of the lens image width, the radial scale in thousandths.
constexpr int kParams = 6;
constexpr double kStep[kParams] = {0.05, 0.05, 0.05, 0.05, 0.05, 0.05}; // derivatives
// The weak pull towards no correction: how far each number may go before it
// costs as much as one block measured to 0.05 deg.
constexpr double kPriorSigma[kParams] = {1.0, 1.0, 1.0, 2.0, 2.0, 3.0};
constexpr double kBound[kParams] = {2.5, 2.5, 2.5, 10.0, 10.0, 10.0};

using Params = std::array<double, kParams>;

SeamCorrection toCorrection(const Params& x)
{
    SeamCorrection c;
    for (int k = 0; k < 3; ++k)
        c.lens1RotDeg[k] = x[k];
    c.lens1Centre[0] = x[3] * 1e-3;
    c.lens1Centre[1] = x[4] * 1e-3;
    c.radialScale = 1.0 + x[5] * 1e-3;
    return c;
}

double sampleGrey(const std::vector<uint8_t>& g, int w, int h, double u, double v)
{
    u = std::clamp(u, 0.0, w - 1.001);
    v = std::clamp(v, 0.0, h - 1.001);
    const int x = static_cast<int>(u), y = static_cast<int>(v);
    const double fx = u - x, fy = v - y;
    const uint8_t* p = &g[static_cast<size_t>(y) * w + x];
    return (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[w] * (1 - fx) + p[w + 1] * fx) * fy;
}

// World direction of strip position (angle off lens 0's axis, azimuth),
// degrees; and back.
Vec3 stripDir(const Mat3& l2w, double thDeg, double aDeg)
{
    const double th = thDeg * kDeg, a = aDeg * kDeg;
    return apply(l2w, Vec3{std::cos(a) * std::sin(th), std::sin(a) * std::sin(th), std::cos(th)});
}

void stripCoords(const Mat3& w2l, const Vec3& dir, double& thDeg, double& aDeg)
{
    const Vec3 d = normalize(apply(w2l, dir));
    thDeg = std::acos(std::clamp(d.z, -1.0, 1.0)) / kDeg;
    aDeg = std::atan2(d.y, d.x) / kDeg;
}

double wrapDeg(double d)
{
    return std::remainder(d, 360.0);
}

// Both lenses' grey along the strip at this stitch geometry; NaN where a
// lens doesn't see the direction.
struct Strip {
    std::vector<float> g[2];
};

Strip sampleStrip(const FisheyeStitcher& st, const SeamFrame& f, double scale)
{
    Strip s;
    const Mat3 l2w = transpose(st.lensProjection(0).worldToLens);
    for (auto& g : s.g)
        g.assign(static_cast<size_t>(kRows) * kCols, std::numeric_limits<float>::quiet_NaN());
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kCols; ++c) {
            const Vec3 w = stripDir(l2w, 90.0 + (r - kStripHalf) * kCellDeg, c * kCellDeg);
            for (int i = 0; i < 2; ++i) {
                double u, v, t;
                if (!st.project(i, w, u, v, t))
                    continue;
                u *= scale, v *= scale;
                if (u < 0 || v < 0 || u > f.w - 1 || v > f.h - 1)
                    continue;
                s.g[i][static_cast<size_t>(r) * kCols + c] =
                    static_cast<float>(sampleGrey(f.grey[i], f.w, f.h, u, v));
            }
        }
    return s;
}

// One block's measured offset of lens 1 against lens 0.
struct Block {
    int index = 0;           // azimuth block, 0..kBlocks-1
    double s[2] = {0, 0};    // lens 1's best match: (azimuth, angle off lens 0's axis), deg
    double info[3] = {0, 0, 0}; // how sharply the match peaks: xx, xy, yy, 1/deg^2
};

// NCC of block b of lens 0 against lens 1 shifted by (dx, dy) cells; -2 when
// too few samples. `tex` (optional): the smaller grey standard deviation.
double blockNcc(const Strip& s, int b, int dx, int dy, double* tex = nullptr)
{
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    int n = 0;
    for (int r = kStripHalf - kCoreHalf; r <= kStripHalf + kCoreHalf; ++r) {
        const float* a = &s.g[0][static_cast<size_t>(r) * kCols];
        const float* q = &s.g[1][static_cast<size_t>(r + dy) * kCols];
        for (int c = b * kBlockCols; c < (b + 1) * kBlockCols; ++c) {
            const float x = a[c], y = q[(c + dx + kCols) % kCols];
            if (std::isnan(x) || std::isnan(y))
                continue;
            sx += x, sy += y, sxx += x * x, syy += y * y, sxy += x * y;
            ++n;
        }
    }
    constexpr int kCore = (2 * kCoreHalf + 1) * kBlockCols;
    if (n < kCore * 9 / 10)
        return -2.0;
    const double mx = sx / n, my = sy / n;
    const double vx = std::max(sxx / n - mx * mx, 0.0), vy = std::max(syy / n - my * my, 0.0);
    if (tex)
        *tex = std::sqrt(std::min(vx, vy));
    if (vx < 1e-6 || vy < 1e-6)
        return 0.0;
    return (sxy / n - mx * my) / std::sqrt(vx * vy);
}

// The textured blocks of one frame, lens 1 searched over +-search cells.
std::vector<Block> measureBlocks(const Strip& s, int search)
{
    std::vector<Block> out;
    const int n = 2 * search + 1;
    std::vector<double> grid(static_cast<size_t>(n) * n);
    for (int b = 0; b < kBlocks; ++b) {
        double tex = 0;
        if (blockNcc(s, b, 0, 0, &tex) < -1.5 || tex < kMinTexture)
            continue;
        int bx = 0, by = 0;
        double best = -2.0;
        for (int dy = -search; dy <= search; ++dy)
            for (int dx = -search; dx <= search; ++dx) {
                const double v = blockNcc(s, b, dx, dy);
                grid[static_cast<size_t>(dy + search) * n + dx + search] = v;
                if (v > best)
                    best = v, bx = dx, by = dy;
            }
        if (best < kMinPeak || std::abs(bx) == search || std::abs(by) == search)
            continue;
        const auto at = [&](int dx, int dy) {
            return grid[static_cast<size_t>(by + dy + search) * n + bx + dx + search];
        };
        // Quadratic around the peak, per cell: gradient and curvature.
        const double gx = 0.5 * (at(1, 0) - at(-1, 0)), gy = 0.5 * (at(0, 1) - at(0, -1));
        const double hxx = at(1, 0) - 2 * best + at(-1, 0);
        const double hyy = at(0, 1) - 2 * best + at(0, -1);
        const double hxy = 0.25 * (at(1, 1) - at(1, -1) - at(-1, 1) + at(-1, -1));
        const double det = hxx * hyy - hxy * hxy;
        if (!(hxx < 0 && hyy < 0 && det > 0))
            continue; // not a clean peak
        const double ox = std::clamp(-(hyy * gx - hxy * gy) / det, -1.0, 1.0);
        const double oy = std::clamp(-(hxx * gy - hxy * gx) / det, -1.0, 1.0);
        Block blk;
        blk.index = b;
        blk.s[0] = (bx + ox) * kCellDeg;
        blk.s[1] = (by + oy) * kCellDeg;
        const double perDeg2 = 1.0 / (kCellDeg * kCellDeg);
        blk.info[0] = -hxx * perDeg2;
        blk.info[1] = -hxy * perDeg2;
        blk.info[2] = -hyy * perDeg2;
        out.push_back(blk);
    }
    return out;
}

// Every frame's blocks at this stitch geometry, frames measured in parallel.
std::vector<Block> measureAll(const FisheyeStitcher& st, const std::vector<SeamFrame>& frames,
                              double scale, int search)
{
    std::vector<std::vector<Block>> per(frames.size());
    std::vector<std::thread> pool;
    for (size_t i = 0; i < frames.size(); ++i)
        pool.emplace_back([&, i] { per[i] = measureBlocks(sampleStrip(st, frames[i], scale), search); });
    for (auto& t : pool)
        t.join();
    std::vector<Block> all;
    for (auto& p : per)
        all.insert(all.end(), p.begin(), p.end());
    return all;
}

// Per azimuth block, at the seam (90 deg off lens 0's axis): the unit
// direction parallax shifts lens 1's match in (something nearer than
// infinity), and how each fitted number shifts it (deg per unit).
struct BlockModel {
    double v[2] = {0, 0};
    double J[2][kParams] = {};
};

bool buildModels(const InsvCalibration& cal, const StitchParams& base, const Params& x, int streamW,
                 int streamH, std::vector<BlockModel>& out)
{
    StitchParams p = base;
    p.seam = toCorrection(x);
    FisheyeStitcher st;
    if (!st.configure(cal, streamW, streamH, p))
        return false;
    FisheyeStitcher stk[kParams];
    for (int k = 0; k < kParams; ++k) {
        Params xk = x;
        xk[k] += kStep[k];
        p.seam = toCorrection(xk);
        if (!stk[k].configure(cal, streamW, streamH, p))
            return false;
    }
    const Mat3 w2l = st.lensProjection(0).worldToLens;
    const Mat3 l2w = transpose(w2l);
    out.assign(kBlocks, BlockModel{});
    for (int b = 0; b < kBlocks; ++b) {
        const double a = (b + 0.5) * kBlockCols * kCellDeg;
        const Vec3 d = stripDir(l2w, 90.0, a);

        // Parallax: the point 1 m out along d, as each lens sees it.
        double th0, a0, th1, a1;
        stripCoords(w2l, st.lensRay(0, d, 1.0), th0, a0);
        stripCoords(w2l, st.lensRay(1, d, 1.0), th1, a1);
        const double pv[2] = {wrapDeg(a1 - a0), th1 - th0};
        const double pn = std::hypot(pv[0], pv[1]);
        if (pn > 1e-9) {
            out[b].v[0] = pv[0] / pn;
            out[b].v[1] = pv[1] / pn;
        }

        // Each lens's image position against the strip position (px per
        // deg), to turn a change of the projection into a strip shift.
        double J0[2][kParams], J1[2][kParams];
        for (int i = 0; i < 2; ++i) {
            double u, v, t, ua, va, ut, vt;
            constexpr double e = 0.05;
            if (!st.project(i, d, u, v, t) || !st.project(i, stripDir(l2w, 90.0, a + e), ua, va, t) ||
                !st.project(i, stripDir(l2w, 90.0 + e, a), ut, vt, t))
                return false;
            const double m00 = (ua - u) / e, m01 = (ut - u) / e;
            const double m10 = (va - v) / e, m11 = (vt - v) / e;
            const double det = m00 * m11 - m01 * m10;
            if (std::abs(det) < 1e-12)
                return false;
            for (int k = 0; k < kParams; ++k) {
                double uk, vk;
                if (!stk[k].project(i, d, uk, vk, t))
                    return false;
                const double du = uk - u, dv = vk - v;
                double(*J)[kParams] = i == 0 ? J0 : J1;
                J[0][k] = (m11 * du - m01 * dv) / det / kStep[k];
                J[1][k] = (-m10 * du + m00 * dv) / det / kStep[k];
            }
        }
        // Lens i's picture moves with the projection; the match is lens 1's
        // against lens 0's.
        for (int r = 0; r < 2; ++r)
            for (int k = 0; k < kParams; ++k)
                out[b].J[r][k] = J1[r][k] - J0[r][k];
    }
    return true;
}

// A block's offset with the parallax part left out: past the fit, a shift
// the way parallax goes is something near, not the calibration.
double offsetOf(const BlockModel& m, const double r[2])
{
    const double along = r[0] * m.v[0] + r[1] * m.v[1];
    if (along <= 0)
        return std::hypot(r[0], r[1]);
    return std::hypot(r[0] - along * m.v[0], r[1] - along * m.v[1]);
}

// Blocks whose match peaks in every direction (not an edge): those the RMS
// offset reports.
bool wellConditioned(const Block& blk)
{
    const double tr = blk.info[0] + blk.info[2];
    const double det = blk.info[0] * blk.info[2] - blk.info[1] * blk.info[1];
    const double disc = std::sqrt(std::max(0.25 * tr * tr - det, 0.0));
    const double lo = 0.5 * tr - disc, hi = 0.5 * tr + disc;
    return lo > 0.25 * hi;
}

double rmsOffset(const std::vector<Block>& blocks, const std::vector<BlockModel>& models,
                 const double* delta)
{
    double sum = 0;
    int n = 0;
    for (const Block& blk : blocks) {
        if (!wellConditioned(blk))
            continue;
        const BlockModel& m = models[blk.index];
        double r[2] = {blk.s[0], blk.s[1]};
        if (delta)
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < kParams; ++k)
                    r[c] -= m.J[c][k] * delta[k];
        const double o = offsetOf(m, r);
        sum += o * o;
        ++n;
    }
    return n ? std::sqrt(sum / n) : -1.0;
}

// Solves A x = b for symmetric positive-definite A (Cholesky); false when
// it isn't.
bool solveSpd(double A[kParams][kParams], const double b[kParams], double x[kParams])
{
    double L[kParams][kParams] = {};
    for (int i = 0; i < kParams; ++i)
        for (int j = 0; j <= i; ++j) {
            double s = A[i][j];
            for (int k = 0; k < j; ++k)
                s -= L[i][k] * L[j][k];
            if (i == j) {
                if (s <= 0)
                    return false;
                L[i][i] = std::sqrt(s);
            } else {
                L[i][j] = s / L[j][j];
            }
        }
    double y[kParams];
    for (int i = 0; i < kParams; ++i) {
        double s = b[i];
        for (int k = 0; k < i; ++k)
            s -= L[i][k] * y[k];
        y[i] = s / L[i][i];
    }
    for (int i = kParams - 1; i >= 0; --i) {
        double s = y[i];
        for (int k = i + 1; k < kParams; ++k)
            s -= L[k][i] * x[k];
        x[i] = s / L[i][i];
    }
    return true;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// One round's update of `x` from blocks measured at `x`: iteratively
// reweighted least squares, the across-the-seam part one-sided against
// parallax, plus the pull towards no correction.
bool fitRound(const std::vector<Block>& blocks, const std::vector<BlockModel>& models,
              const Params& x, double delta[kParams])
{
    std::vector<double> traces;
    for (const Block& blk : blocks)
        traces.push_back(0.5 * (blk.info[0] + blk.info[2]));
    const double infoScale = median(traces);

    std::vector<double> weight(blocks.size(), 1.0);
    std::vector<char> parallax(blocks.size(), 0); // its shift along v is left to parallax
    std::fill(delta, delta + kParams, 0.0);
    for (int it = 0; it < 12; ++it) {
        double A[kParams][kParams] = {}, g[kParams] = {};
        for (int k = 0; k < kParams; ++k) {
            const double p = infoScale * std::pow(0.05 / kPriorSigma[k], 2);
            A[k][k] += p;
            g[k] -= p * x[k];
        }
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block& blk = blocks[i];
            const BlockModel& m = models[blk.index];
            double W[2][2] = {{blk.info[0], blk.info[1]}, {blk.info[1], blk.info[2]}};
            if (parallax[i]) {
                // Leave the shift along v free: what's left is the
                // information across it (the Schur complement in the
                // (v, n) basis).
                const double n[2] = {-m.v[1], m.v[0]};
                const double Ivv = m.v[0] * (W[0][0] * m.v[0] + W[0][1] * m.v[1]) +
                                   m.v[1] * (W[1][0] * m.v[0] + W[1][1] * m.v[1]);
                const double Ivn = m.v[0] * (W[0][0] * n[0] + W[0][1] * n[1]) +
                                   m.v[1] * (W[1][0] * n[0] + W[1][1] * n[1]);
                const double Inn = n[0] * (W[0][0] * n[0] + W[0][1] * n[1]) +
                                   n[1] * (W[1][0] * n[0] + W[1][1] * n[1]);
                const double In = Ivv > 1e-12 ? Inn - Ivn * Ivn / Ivv : Inn;
                for (int r = 0; r < 2; ++r)
                    for (int c = 0; c < 2; ++c)
                        W[r][c] = In * n[r] * n[c];
            }
            for (int r = 0; r < 2; ++r)
                for (int c = 0; c < 2; ++c)
                    W[r][c] *= weight[i];
            double WJ[2][kParams];
            for (int r = 0; r < 2; ++r)
                for (int k = 0; k < kParams; ++k)
                    WJ[r][k] = W[r][0] * m.J[0][k] + W[r][1] * m.J[1][k];
            for (int k = 0; k < kParams; ++k) {
                for (int l = 0; l < kParams; ++l)
                    A[k][l] += m.J[0][k] * WJ[0][l] + m.J[1][k] * WJ[1][l];
                g[k] += WJ[0][k] * blk.s[0] + WJ[1][k] * blk.s[1];
            }
        }
        if (!solveSpd(A, g, delta))
            return false;

        // Residuals: noise from the part across v (parallax can't make it),
        // then the weights. The first pass has no one-sided rule yet --
        // near blocks pull it, the next passes let them go.
        std::vector<double> across(blocks.size()), along(blocks.size());
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block& blk = blocks[i];
            const BlockModel& m = models[blk.index];
            double r[2] = {blk.s[0], blk.s[1]};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < kParams; ++k)
                    r[c] -= m.J[c][k] * delta[k];
            along[i] = r[0] * m.v[0] + r[1] * m.v[1];
            across[i] = std::abs(-r[0] * m.v[1] + r[1] * m.v[0]);
        }
        const double sigma = std::max(1.4826 * median(across), 0.02);
        const double tau = it == 0 ? 0.5 : std::max(3.0 * sigma, 0.06);
        for (size_t i = 0; i < blocks.size(); ++i) {
            parallax[i] = along[i] > tau;
            const double r = parallax[i] ? across[i] : std::hypot(along[i], across[i]);
            const double c = std::max(2.5 * sigma, 0.05);
            weight[i] = r > c ? c / r : 1.0;
        }
    }
    return true;
}
} // namespace

int64_t SnapToKeyframeMs(int64_t ms, double fps)
{
    if (fps <= 1.0)
        return ms;
    const double gopMs = 60.0 * 1000.0 / fps;
    return static_cast<int64_t>(std::llround(std::round(ms / gopMs) * gopMs));
}

SeamFrame MakeSeamFrame(const Image& lens0, const Image& lens1)
{
    SeamFrame f;
    f.w = lens0.w / 2;
    f.h = lens0.h / 2;
    const Image* im[2] = {&lens0, &lens1};
    for (int i = 0; i < 2; ++i) {
        f.grey[i].assign(static_cast<size_t>(f.w) * f.h, 0);
        if (im[i]->w != lens0.w || im[i]->h != lens0.h)
            continue;
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                double l = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const uint8_t* p =
                            &im[i]->rgb[((static_cast<size_t>(2 * y + dy)) * im[i]->w + 2 * x + dx) * 3];
                        l += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                    }
                f.grey[i][static_cast<size_t>(y) * f.w + x] = static_cast<uint8_t>(l * 0.25 + 0.5);
            }
    }
    return f;
}

SeamAlignmentResult EstimateSeamAlignment(const InsvCalibration& cal, const StitchParams& params,
                                          int streamW, int streamH,
                                          const std::vector<SeamFrame>& frames,
                                          const std::atomic<bool>* cancel)
{
    SeamAlignmentResult out;
    if (cal.lensesExt.size() != 2) {
        out.message = "no extended (MEI) calibration";
        return out;
    }
    if (frames.empty()) {
        out.message = "no frames";
        return out;
    }
    const double scale = static_cast<double>(frames[0].w) / streamW;
    const auto cancelled = [&] {
        if (cancel && cancel->load()) {
            out.message = "cancelled";
            return true;
        }
        return false;
    };

    Params x{};
    std::vector<Block> blocks;
    std::vector<BlockModel> models;
    for (int round = 0; round <= kRounds; ++round) {
        if (cancelled())
            return out;
        StitchParams p = params;
        p.seam = toCorrection(x);
        FisheyeStitcher st;
        if (!st.configure(cal, streamW, streamH, p) ||
            !buildModels(cal, params, x, streamW, streamH, models)) {
            out.message = "stitch geometry failed";
            return out;
        }
        // The first and the last round search wide, so the offsets before
        // and after are measured alike (and as MeasureSeamOffset does).
        const bool wide = round == 0 || round == kRounds;
        blocks = measureAll(st, frames, scale, wide ? kSearchFirst : kSearchLater);
        if (cancelled())
            return out;
        if (round == 0) {
            out.offsetBeforeDeg = rmsOffset(blocks, models, nullptr);
            if (static_cast<int>(blocks.size()) < kMinBlocks) {
                out.message = "no textured seam content";
                return out;
            }
        }
        if (round == kRounds) {
            out.offsetAfterDeg = rmsOffset(blocks, models, nullptr);
            break;
        }
        double delta[kParams];
        if (static_cast<int>(blocks.size()) < kMinBlocks || !fitRound(blocks, models, x, delta)) {
            out.message = "no textured seam content";
            return out;
        }
        for (int k = 0; k < kParams; ++k)
            x[k] = std::clamp(x[k] + delta[k], -kBound[k], kBound[k]);
    }

    out.ok = true;
    out.correction = toCorrection(x);
    out.frames = static_cast<int>(frames.size());
    out.blocks = static_cast<int>(blocks.size());
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "lens 1 yaw %+.2f pitch %+.2f roll %+.2f deg, centre %+.3f %+.3f %%, "
                  "scale %+.3f %% (seam offset %.2f -> %.2f deg, %d blocks)",
                  x[0], x[1], x[2], x[3] * 0.1, x[4] * 0.1, x[5] * 0.1,
                  out.offsetBeforeDeg, out.offsetAfterDeg, out.blocks);
    out.message = buf;
    return out;
}

double MeasureSeamOffset(const InsvCalibration& cal, const StitchParams& params, int streamW,
                         int streamH, const std::vector<SeamFrame>& frames)
{
    if (cal.lensesExt.size() != 2 || frames.empty())
        return -1.0;
    FisheyeStitcher st;
    if (!st.configure(cal, streamW, streamH, params))
        return -1.0;
    // The parallax direction and derivatives at this geometry; only v is used.
    StitchParams base = params;
    base.seam = {};
    const SeamCorrection& c = params.seam;
    const Params x{c.lens1RotDeg[0], c.lens1RotDeg[1], c.lens1RotDeg[2],
                   c.lens1Centre[0] * 1e3, c.lens1Centre[1] * 1e3, (c.radialScale - 1.0) * 1e3};
    std::vector<BlockModel> models;
    if (!buildModels(cal, base, x, streamW, streamH, models))
        return -1.0;
    const double scale = static_cast<double>(frames[0].w) / streamW;
    return rmsOffset(measureAll(st, frames, scale, kSearchFirst), models, nullptr);
}

} // namespace insv
