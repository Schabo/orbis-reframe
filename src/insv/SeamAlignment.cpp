// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SeamAlignment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <thread>

namespace insv {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

// Strip geometry: rows are the angle off the lens-0 90 deg line, columns the
// azimuth around it.
constexpr double kHalfBandDeg = 2.5;
constexpr double kRowStepDeg = 0.1;
constexpr double kColStepDeg = 0.2;
constexpr int kRows = static_cast<int>(2 * kHalfBandDeg / kRowStepDeg) + 1;
constexpr int kCols = static_cast<int>(360.0 / kColStepDeg);
constexpr int kBlocks = 36; // 10 deg azimuth blocks
constexpr double kMaxAdjustDeg = 1.5;

double sampleGrey(const std::vector<uint8_t>& g, int w, int h, double u, double v)
{
    u = std::clamp(u, 0.0, w - 1.001);
    v = std::clamp(v, 0.0, h - 1.001);
    const int x = static_cast<int>(u), y = static_cast<int>(v);
    const double fx = u - x, fy = v - y;
    const uint8_t* p = &g[static_cast<size_t>(y) * w + x];
    return (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[w] * (1 - fx) + p[w + 1] * fx) * fy;
}

// Mean NCC over textured azimuth blocks of the seam strip, for one stitcher.
double stripScore(const FisheyeStitcher& st, const SeamFrame& f, double scale)
{
    const Mat3 l2w = transpose(st.lensProjection(0).worldToLens);
    double sum[kBlocks][5] = {};
    long n[kBlocks] = {};
    for (int r = 0; r < kRows; ++r) {
        const double th = (90.0 - kHalfBandDeg + r * kRowStepDeg) * kDeg;
        const double st0 = std::sin(th), ct0 = std::cos(th);
        for (int c = 0; c < kCols; ++c) {
            const double a = c * kColStepDeg * kDeg;
            const Vec3 w = apply(l2w, Vec3{std::cos(a) * st0, std::sin(a) * st0, ct0});
            double u0, v0, u1, v1, t;
            if (!st.project(0, w, u0, v0, t) || !st.project(1, w, u1, v1, t))
                continue;
            u0 *= scale, v0 *= scale, u1 *= scale, v1 *= scale;
            if (u0 < 0 || v0 < 0 || u1 < 0 || v1 < 0 || u0 > f.w - 1 || v0 > f.h - 1 ||
                u1 > f.w - 1 || v1 > f.h - 1)
                continue;
            const double x = sampleGrey(f.grey[0], f.w, f.h, u0, v0);
            const double y = sampleGrey(f.grey[1], f.w, f.h, u1, v1);
            double* s = sum[c * kBlocks / kCols];
            s[0] += x, s[1] += y, s[2] += x * x, s[3] += y * y, s[4] += x * y;
            ++n[c * kBlocks / kCols];
        }
    }
    double acc = 0;
    int blocks = 0;
    for (int b = 0; b < kBlocks; ++b) {
        if (n[b] < 100)
            continue;
        const double* s = sum[b];
        const double va = s[2] - s[0] * s[0] / n[b], vb = s[3] - s[1] * s[1] / n[b];
        if (va < 4.0 * n[b] || vb < 4.0 * n[b]) // no texture (std < 2 grey levels)
            continue;
        acc += (s[4] - s[0] * s[1] / n[b]) / std::sqrt(va * vb);
        ++blocks;
    }
    return blocks ? acc / blocks : -2.0;
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

    auto score = [&](const double adj[3]) {
        StitchParams p = params;
        for (int k = 0; k < 3; ++k)
            p.lens1AdjustDeg[k] = adj[k];
        FisheyeStitcher st;
        if (!st.configure(cal, streamW, streamH, p))
            return -2.0;
        std::vector<double> s(frames.size(), -2.0);
        std::vector<std::thread> pool;
        for (size_t i = 0; i < frames.size(); ++i)
            pool.emplace_back([&, i] { s[i] = stripScore(st, frames[i], scale); });
        for (auto& t : pool)
            t.join();
        double acc = 0;
        int n = 0;
        for (double v : s)
            if (v > -1.5)
                acc += v, ++n;
        return n ? acc / n : -2.0;
    };

    double best[3] = {0, 0, 0};
    double bestScore = score(best);
    out.scoreBefore = bestScore;
    if (bestScore < -1.5) {
        out.message = "no textured seam content";
        return out;
    }
    for (double step : {0.2, 0.1, 0.05}) {
        for (bool moved = true; moved;) {
            moved = false;
            for (int ax = 0; ax < 3; ++ax) {
                for (int sign : {-1, 1}) {
                    if (cancel && cancel->load()) {
                        out.message = "cancelled";
                        return out;
                    }
                    double cand[3] = {best[0], best[1], best[2]};
                    cand[ax] += sign * step;
                    if (std::abs(cand[ax]) > kMaxAdjustDeg)
                        continue;
                    const double s = score(cand);
                    if (s > bestScore + 1e-4) {
                        bestScore = s;
                        std::copy(cand, cand + 3, best);
                        moved = true;
                    }
                }
            }
        }
    }
    out.ok = true;
    std::copy(best, best + 3, out.adjustDeg);
    out.scoreAfter = bestScore;
    out.frames = static_cast<int>(frames.size());
    char buf[160];
    std::snprintf(buf, sizeof buf, "lens 1 yaw %+.2f pitch %+.2f roll %+.2f deg (seam match %.3f -> %.3f)",
                  best[0], best[1], best[2], out.scoreBefore, out.scoreAfter);
    out.message = buf;
    return out;
}

} // namespace insv
