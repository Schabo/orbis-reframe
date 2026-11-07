#include "SeamFlow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

namespace {

float AnchorHash(uint32_t ix, uint32_t iy, uint32_t octave)
{
    uint32_t h = ix * 374761393u + iy * 668265263u + octave * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xffffu) / 65535.0f * 2.0f - 1.0f;
}

// Box means over +-r of a rows x cols plane: x wraps round, rows past the
// ends repeat them.
void BoxMean(const std::vector<float>& in, int C, int R, int r, std::vector<float>& out)
{
    std::vector<float> rows(in.size());
    for (int y = 0; y < R; ++y) {
        const float* p = in.data() + static_cast<size_t>(y) * C;
        for (int x = 0; x < C; ++x) {
            float sum = 0;
            for (int i = -r; i <= r; ++i)
                sum += p[((x + i) % C + C) % C];
            rows[static_cast<size_t>(y) * C + x] = sum;
        }
    }
    out.resize(in.size());
    const float area = static_cast<float>((2 * r + 1) * (2 * r + 1));
    for (int y = 0; y < R; ++y)
        for (int x = 0; x < C; ++x) {
            float sum = 0;
            for (int j = -r; j <= r; ++j)
                sum += rows[static_cast<size_t>(std::clamp(y + j, 0, R - 1)) * C + x];
            out[static_cast<size_t>(y) * C + x] = sum / area;
        }
}

} // namespace

float SeamFlowAnchorNoise(int x, int y)
{
    float sum = 0;
    for (int o = 0; o < 3; ++o) {
        const float cell = static_cast<float>(4 << o);
        const float gx = x / cell, gy = y / cell;
        const float fx0 = std::floor(gx), fy0 = std::floor(gy);
        const uint32_t ix = static_cast<uint32_t>(fx0), iy = static_cast<uint32_t>(fy0);
        float fx = gx - fx0, fy = gy - fy0;
        fx = fx * fx * (3 - 2 * fx);
        fy = fy * fy * (3 - 2 * fy);
        const uint32_t oc = static_cast<uint32_t>(o);
        const float a = AnchorHash(ix, iy, oc) * (1 - fx) + AnchorHash(ix + 1, iy, oc) * fx;
        const float b = AnchorHash(ix, iy + 1, oc) * (1 - fx) + AnchorHash(ix + 1, iy + 1, oc) * fx;
        sum += a * (1 - fy) + b * fy;
    }
    return sum / 0.75f;
}

void AnchorSeamFlowStrips(const SeamFlowStrips& in, SeamFlowStrips& out)
{
    const SeamFlowGrid& g = in.grid;
    const int C = g.cols, R = g.rows, r = kSeamFlowAnchorBoxPx;
    out.grid = g;
    auto smooth = [](double a, double b, double x) {
        const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    std::vector<float> v, mean, d, var;
    for (int l = 0; l < 2; ++l) {
        const std::vector<uint8_t>& src = in.grey[l];
        v.assign(src.begin(), src.end());
        BoxMean(v, C, R, r, mean);
        d.resize(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            d[i] = (v[i] - mean[i]) * (v[i] - mean[i]);
        BoxMean(d, C, R, r, var);
        std::vector<uint8_t>& dst = out.grey[l];
        dst.resize(src.size());
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < C; ++x) {
                const size_t i = static_cast<size_t>(y) * C + x;
                const double k = (1.0 - smooth(kSeamFlowAnchorTexLo, kSeamFlowAnchorTexHi, std::sqrt(var[i]))) *
                                 smooth(kSeamFlowAnchorBrightLo, kSeamFlowAnchorBrightHi, mean[i]);
                const double o = v[i] + kSeamFlowAnchorAmp * k * SeamFlowAnchorNoise(x, y);
                dst[i] = static_cast<uint8_t>(std::clamp(std::floor(o + 0.5), 0.0, 255.0));
            }
    }
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
    out.fade.clear();
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
