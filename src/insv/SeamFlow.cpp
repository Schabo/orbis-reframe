#include "SeamFlow.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>

namespace insv {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;
} // namespace

double SeamFlowGrid::phiOf(double x) const { return (x + 0.5) * 2.0 * kPi / cols; }
double SeamFlowGrid::eOf(double y) const { return stripRad - y * 2.0 * stripRad / (rows - 1); }
double SeamFlowGrid::xOf(double phi) const { return phi * cols / (2.0 * kPi) - 0.5; }
double SeamFlowGrid::yOf(double e) const { return (stripRad - e) * (rows - 1) / (2.0 * stripRad); }

SeamFlowGrid SeamFlowGridFor(const FisheyeStitcher& st)
{
    SeamFlowGrid g;
    if (!st.ready())
        return g;
    const StitchGpuParams p = st.gpuParams();
    // Both lenses see all round the seam circle at elevation e (99 % of it:
    // a sample inside the frame, edge weight left).
    auto covered = [&](double eDeg) {
        const int n = 720;
        int ok = 0;
        for (int k = 0; k < n; ++k) {
            const Vec3 dir = st.seamDir(2.0 * kPi * (k + 0.5) / n, eDeg * kDeg);
            bool both = true;
            for (int i = 0; i < 2 && both; ++i) {
                double u, v, th;
                both = st.project(i, dir, u, v, th) && u >= 0 && v >= 0 && u <= p.fisheyeW - 1.0 &&
                       v <= p.fisheyeH - 1.0 && st.edgeWeight(th) > 0.05;
            }
            ok += both;
        }
        return ok * 100 >= n * 99;
    };
    double cover = 0;
    while (cover < kSeamFlowStripMaxDeg && covered(cover + 0.25) && covered(-(cover + 0.25)))
        cover += 0.25;
    if (cover < 1.0)
        return g;
    g.stripRad = static_cast<float>(cover * kDeg);
    g.reachRad = static_cast<float>(kSeamFlowReachDeg * kDeg);
    g.cols = static_cast<int>(std::lround(360.0 * kSeamFlowPxDeg));
    g.rows = static_cast<int>(std::lround(2.0 * cover * kSeamFlowPxDeg)) + 1;
    return g;
}

double SeamFlowShare(double e, double reach)
{
    if (e >= reach)
        return 0.0;
    if (e <= -reach)
        return 1.0;
    return 0.5 * (1.0 - std::sin(0.5 * kPi * e / reach));
}

void MeasureSeamFlowStrips(const FisheyeStitcher& st, const SeamFlowGrid& grid, const Image& lens0,
                           const Image& lens1, SeamFlowStrips& out)
{
    out.grid = grid;
    const Image* lim[2] = {&lens0, &lens1};
    for (int i = 0; i < 2; ++i)
        out.grey[i].assign(static_cast<size_t>(grid.rows) * grid.cols, 0);
    if (!grid.valid())
        return;
    const unsigned hw = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < hw; ++t)
        pool.emplace_back([&, t] {
            for (int y = static_cast<int>(t); y < grid.rows; y += static_cast<int>(hw)) {
                const double e = grid.eOf(y);
                for (int x = 0; x < grid.cols; ++x) {
                    const Vec3 dir = st.seamDir(grid.phiOf(x), e);
                    for (int i = 0; i < 2; ++i) {
                        double u, v, th;
                        if (!st.project(i, dir, u, v, th) || u < 0 || v < 0 || u > lim[i]->w - 1.0 ||
                            v > lim[i]->h - 1.0)
                            continue;
                        const auto s = lim[i]->sample(u, v);
                        const double l = 0.299 * s[0] + 0.587 * s[1] + 0.114 * s[2];
                        out.grey[i][static_cast<size_t>(y) * grid.cols + x] =
                            static_cast<uint8_t>(std::clamp(l + 0.5, 0.0, 255.0));
                    }
                }
            }
        });
    for (auto& th : pool)
        th.join();
}

void SeamFlowField::at(double x, double y, float out[4]) const
{
    const int C = grid.cols, R = grid.rows;
    y = std::clamp(y, 0.0, R - 1.0);
    const int y0 = std::min(static_cast<int>(y), R - 2);
    const double ay = y - y0;
    const double fx = std::floor(x);
    const double ax = x - fx;
    const int x0 = ((static_cast<int>(fx) % C) + C) % C;
    const int x1 = (x0 + 1) % C;
    const float* r0 = v.data() + static_cast<size_t>(y0) * C * 4;
    const float* r1 = r0 + static_cast<size_t>(C) * 4;
    for (int c = 0; c < 4; ++c) {
        const double top = r0[x0 * 4 + c] * (1 - ax) + r0[x1 * 4 + c] * ax;
        const double bot = r1[x0 * 4 + c] * (1 - ax) + r1[x1 * 4 + c] * ax;
        out[c] = static_cast<float>(top * (1 - ay) + bot * ay);
    }
}

void SeamFlowWindow::add(SeamFlowField f)
{
    if (!frames_.empty() && !(frames_.back().grid == f.grid))
        frames_.clear();
    frames_.push_back(std::move(f));
    while (static_cast<int>(frames_.size()) > kSeamFlowFrames)
        frames_.pop_front();
}

void SeamFlowWindow::solve(SeamFlowField& out) const
{
    out.v.clear();
    if (frames_.empty())
        return;
    out.grid = frames_.back().grid;
    out.v.assign(frames_.back().v.size(), 0.0f);
    for (const SeamFlowField& f : frames_)
        for (size_t j = 0; j < out.v.size(); ++j)
            out.v[j] += f.v[j];
    const float w = 1.0f / static_cast<float>(frames_.size());
    for (float& x : out.v)
        x *= w;
}

} // namespace insv
