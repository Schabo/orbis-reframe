// The new stitcher's seam flow on the GPU (seam_flow.comp, through
// render::VulkanFisheyeStitcher::estimateSeamFlow / readSeamFlow) on
// synthetic strips: a whole-strip shift is found both ways, across the
// strips' azimuth wrap too, and despite a brightness difference between the
// lenses; a shift in one part of the strip stays there; the window averages
// the frames it holds and starts again after a reset; the fade between the
// lenses widens where one lens is brighter near the seam, and only there, as
// the CPU's (insv::SeamFadeWindow) on the same strips and field. Where the CPU's
// estimator is built (OpenCV), the two agree on a near thing crossing the
// seam.
// Standalone; the exit code is the failure count, 77 (skipped) without a
// Vulkan device.

#include "VulkanFisheyeStitcher.h"

#include <SeamFade.h>

#ifdef ORBIS_SEAM_FLOW
#include <SeamFlowEstimator.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// The X5's grid: 7 px/deg round the circle, +-5.5 deg across.
insv::SeamFlowGrid Grid()
{
    insv::SeamFlowGrid g;
    g.cols = 2520;
    g.rows = 78;
    g.stripRad = static_cast<float>(5.5 * std::numbers::pi / 180.0);
    g.reachRad = static_cast<float>(8.0 * std::numbers::pi / 180.0);
    return g;
}

// A smooth random texture over the grid, wrapping round in x: value noise
// (random grey levels on a lattice, smoothly interpolated) at two scales,
// sampled at any (x, y).
struct Texture {
    struct Octave {
        int cell; // lattice spacing, strip pixels (divides the columns)
        double amp;
        std::vector<double> v; // (cols / cell) x rows-of-lattice
        int nx = 0, ny = 0;
    };
    std::vector<Octave> octaves;
    explicit Texture(int cols, unsigned seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        for (auto [cell, amp] : {std::pair{6, 40.0}, std::pair{18, 50.0}}) {
            Octave o{cell, amp, {}, cols / cell, 200 / cell + 3};
            o.v.resize(static_cast<size_t>(o.nx) * o.ny);
            for (double& x : o.v)
                x = u(rng);
            octaves.push_back(std::move(o));
        }
    }
    static double smooth(double a) { return a * a * (3 - 2 * a); }
    double at(double x, double y) const
    {
        double v = 128.0;
        for (const Octave& o : octaves) {
            const double gx = x / o.cell, gy = (y + 50.0) / o.cell; // y may go a little negative
            const double fx = std::floor(gx), fy = std::floor(gy);
            const double ax = smooth(gx - fx), ay = smooth(gy - fy);
            const int x0 = ((static_cast<int>(fx) % o.nx) + o.nx) % o.nx, x1 = (x0 + 1) % o.nx;
            const int y0 = std::clamp(static_cast<int>(fy), 0, o.ny - 2), y1 = y0 + 1;
            auto L = [&](int xi, int yi) { return o.v[static_cast<size_t>(yi) * o.nx + xi]; };
            const double t = L(x0, y0) + (L(x1, y0) - L(x0, y0)) * ax;
            const double b = L(x0, y1) + (L(x1, y1) - L(x0, y1)) * ax;
            v += o.amp * (t + (b - t) * ay);
        }
        return v;
    }
};

uint8_t Grey(double v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

// Lens 0 shows the texture; lens 1 the same thing `shift(x, y)` strip
// pixels further on (so F01 = shift), `offset` grey levels brighter.
template <class Shift>
insv::SeamFlowStrips Strips(const Texture& t, Shift shift, double offset = 0.0)
{
    insv::SeamFlowStrips s;
    s.grid = Grid();
    const int C = s.grid.cols, R = s.grid.rows;
    for (int i = 0; i < 2; ++i)
        s.grey[i].resize(static_cast<size_t>(C) * R);
    for (int y = 0; y < R; ++y)
        for (int x = 0; x < C; ++x) {
            const size_t k = static_cast<size_t>(y) * C + x;
            s.grey[0][k] = Grey(t.at(x, y));
            double dx, dy;
            shift(x, y, dx, dy);
            s.grey[1][k] = Grey(t.at(x - dx, y - dy) + offset);
        }
    return s;
}

// The field's F01 / F10 at (x, y) in strip pixels.
void FlowPx(const insv::SeamFlowField& f, int x, int y, double f01[2], double f10[2])
{
    const float* v = f.v.data() + (static_cast<size_t>(y) * f.grid.cols + x) * 4;
    const double px = f.grid.cols / (2 * std::numbers::pi);
    const double py = -(f.grid.rows - 1) / (2.0 * f.grid.stripRad);
    f10[0] = v[0] * px;
    f10[1] = v[1] * py;
    f01[0] = v[2] * px;
    f01[1] = v[3] * py;
}

// The median |F01 - want| and |F10 + want| (strip pixels) over the rows
// [y0, y1) and the columns where `use` says.
template <class Use>
double MedianError(const insv::SeamFlowField& f, double wx, double wy, int y0, int y1, Use use)
{
    std::vector<double> e;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < f.grid.cols; ++x) {
            if (!use(x))
                continue;
            double a[2], b[2];
            FlowPx(f, x, y, a, b);
            e.push_back(std::hypot(a[0] - wx, a[1] - wy));
            e.push_back(std::hypot(b[0] + wx, b[1] + wy));
        }
    if (e.empty())
        return 1e9;
    std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end());
    return e[e.size() / 2];
}

bool Estimate(render::VulkanFisheyeStitcher& gpu, const insv::SeamFlowStrips& s,
              insv::SeamFlowField& out)
{
    std::string err;
    if (!gpu.estimateSeamFlow(s, err) || !gpu.readSeamFlow(out, err)) {
        std::printf("FAIL  seam flow: %s\n", err.c_str());
        ++g_failures;
        return false;
    }
    return true;
}

const auto kAll = [](int) { return true; };

void testWholeShift(render::VulkanFisheyeStitcher& gpu, const Texture& t)
{
    // 1.6 px along, 0.7 px across, lens 1 eight grey levels brighter.
    gpu.resetSeamFlow();
    insv::SeamFlowField f;
    if (!Estimate(gpu, Strips(t, [](int, int, double& dx, double& dy) { dx = 1.6, dy = 0.7; }, 8.0), f))
        return;
    CHECK(gpu.seamFlowFrames() == 1);
    CHECK(f.grid.cols == Grid().cols && f.grid.rows == Grid().rows);
    const double e = MedianError(f, 1.6, 0.7, 8, f.grid.rows - 8, kAll);
    std::printf("  whole shift: median error %.3f px\n", e);
    CHECK(e < 0.1);
    // Across the azimuth wrap as anywhere else.
    const double w = MedianError(f, 1.6, 0.7, 8, f.grid.rows - 8,
                                 [&](int x) { return x < 20 || x >= f.grid.cols - 20; });
    std::printf("  at the wrap: median error %.3f px\n", w);
    CHECK(w < 0.15);
}

void testLocalShift(render::VulkanFisheyeStitcher& gpu, const Texture& t)
{
    // A near thing: 4 px along over 300 columns, nothing elsewhere.
    auto inThing = [](int x) { return x >= 1000 && x < 1300; };
    gpu.resetSeamFlow();
    insv::SeamFlowField f;
    if (!Estimate(gpu,
                  Strips(t, [&](int x, int, double& dx, double& dy) { dx = inThing(x) ? 4.0 : 0.0, dy = 0; }),
                  f))
        return;
    const double in = MedianError(f, 4.0, 0, 8, f.grid.rows - 8, [&](int x) { return x >= 1040 && x < 1260; });
    const double out = MedianError(f, 0, 0, 8, f.grid.rows - 8, [&](int x) { return x < 940 || x >= 1360; });
    std::printf("  local shift: inside %.3f px, outside %.3f px\n", in, out);
    CHECK(in < 0.3);
    CHECK(out < 0.05);
}

// The step in F01 along the strip from column a to column b, median over the
// rows away from the strip's edges (strip pixels).
double ColumnStep(const insv::SeamFlowField& f, int a, int b)
{
    std::vector<double> e;
    for (int y = 8; y < f.grid.rows - 8; ++y) {
        double fa[2], fb[2], r[2];
        FlowPx(f, a, y, fa, r);
        FlowPx(f, b, y, fb, r);
        e.push_back(std::hypot(fa[0] - fb[0], fa[1] - fb[1]));
    }
    std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end());
    return e[e.size() / 2];
}

void testShiftAtWrap(render::VulkanFisheyeStitcher& gpu, const Texture& t)
{
    // A near thing across the azimuth wrap: 3 px along at column 0, easing
    // to none 150 columns either side. The field goes on smoothly across
    // the wrap, as it does across the same thing in the middle of the strip
    // -- a step there would tear the picture along that line. The middle's
    // at column 1280, where a propagation stretch starts as one does at 0
    // (seam_flow.comp: every 20 patches of 4 pixels at the finest scale, a
    // quarter of the strip's columns).
    const int C = Grid().cols, M = 1280;
    auto bump = [C](int centre) {
        return [C, centre](int x, int, double& dx, double& dy) {
            const int d = std::abs(((x - centre) % C + C + C / 2) % C - C / 2);
            dx = d < 150 ? 1.5 * (1.0 + std::cos(std::numbers::pi * d / 150.0)) : 0.0;
            dy = 0;
        };
    };
    insv::SeamFlowField wrap, mid;
    gpu.resetSeamFlow();
    if (!Estimate(gpu, Strips(t, bump(0)), wrap))
        return;
    gpu.resetSeamFlow();
    if (!Estimate(gpu, Strips(t, bump(M)), mid))
        return;
    const double atWrap = ColumnStep(wrap, C - 1, 0), atMid = ColumnStep(mid, M - 1, M);
    const double nearWrap = MedianError(wrap, 3.0, 0, 8, wrap.grid.rows - 8, [C](int x) { return x < 4 || x >= C - 4; });
    const double nearMid = MedianError(mid, 3.0, 0, 8, mid.grid.rows - 8, [M](int x) { return std::abs(x - M) < 4; });
    std::printf("  shift at the wrap: step %.3f px (mid-strip %.3f), error %.3f px (mid-strip %.3f)\n",
                atWrap, atMid, nearWrap, nearMid);
    CHECK(atWrap < atMid + 0.02);
    CHECK(nearWrap < nearMid + 0.05);
}

void testWindow(render::VulkanFisheyeStitcher& gpu, const Texture& t)
{
    // Two frames, 1 px and 2 px: the window holds their mean; a reset
    // starts again with the next.
    gpu.resetSeamFlow();
    insv::SeamFlowField f;
    Estimate(gpu, Strips(t, [](int, int, double& dx, double& dy) { dx = 1.0, dy = 0; }), f);
    if (!Estimate(gpu, Strips(t, [](int, int, double& dx, double& dy) { dx = 2.0, dy = 0; }), f))
        return;
    CHECK(gpu.seamFlowFrames() == 2);
    const double mean = MedianError(f, 1.5, 0, 8, f.grid.rows - 8, kAll);
    std::printf("  window of two: median error %.3f px from their mean\n", mean);
    CHECK(mean < 0.15);
    for (int i = 0; i < 6; ++i)
        Estimate(gpu, Strips(t, [](int, int, double& dx, double& dy) { dx = 2.0, dy = 0; }), f);
    CHECK(gpu.seamFlowFrames() == insv::kSeamFlowFrames);
    CHECK(MedianError(f, 2.0, 0, 8, f.grid.rows - 8, kAll) < 0.15); // the 1 px frame gone
    gpu.resetSeamFlow();
    CHECK(gpu.seamFlowFrames() == 0);
    if (!Estimate(gpu, Strips(t, [](int, int, double& dx, double& dy) { dx = -1.0, dy = 0; }), f))
        return;
    CHECK(gpu.seamFlowFrames() == 1);
    CHECK(MedianError(f, -1.0, 0, 8, f.grid.rows - 8, kAll) < 0.15);
}

void testWindowOrder(render::VulkanFisheyeStitcher& gpu, const Texture& t)
{
    // The same five frames give the same field to the bit wherever they sit
    // in the ring: played into (a frame before them pushed out) or measured
    // fresh after a seek.
    auto frame = [&](int i) {
        return Strips(t, [i](int x, int, double& dx, double& dy) {
            dx = 0.3 * i + 0.2 * std::sin(x * 0.01 * (i + 1));
            dy = 0.1 * i;
        });
    };
    insv::SeamFlowField played, fresh;
    gpu.resetSeamFlow();
    for (int i = 0; i < 6; ++i)
        if (!Estimate(gpu, frame(i), played))
            return;
    gpu.resetSeamFlow();
    for (int i = 1; i < 6; ++i)
        if (!Estimate(gpu, frame(i), fresh))
            return;
    CHECK(played.v == fresh.v);
}

// The p90 of |F01| and |F10| (strip pixels) over the rows [y0, y1) and the
// columns [x0, x1).
double P90Flow(const insv::SeamFlowField& f, int x0, int x1, int y0, int y1)
{
    std::vector<double> m;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            double a[2], b[2];
            FlowPx(f, x, y, a, b);
            m.push_back(std::hypot(a[0], a[1]));
            m.push_back(std::hypot(b[0], b[1]));
        }
    std::sort(m.begin(), m.end());
    return m[m.size() * 9 / 10];
}

void testBrightSky(render::VulkanFisheyeStitcher& gpu, const Texture& t)
{
    // A far scene (no shift), but over columns 1000-1500 the strip's upper
    // half is bright sky with next to no texture, and lens 1 alone has the
    // sun's veil over it, a soft bright ramp. Any shift matches there, so
    // the flow would be arbitrary and spread into the scene below it; the
    // sky is anchored, so the flow stays zero in both. A near thing
    // shifted across the seam elsewhere is still joined up.
    auto acrossThing = [](int x) { return x >= 400 && x < 700; };
    insv::SeamFlowStrips s = Strips(t, [&](int x, int, double& dx, double& dy) {
        dx = 0;
        dy = acrossThing(x) ? 6.0 : 0.0;
    });
    const int C = Grid().cols, R = Grid().rows, sky = R / 2;
    for (int y = 0; y < sky; ++y)
        for (int x = 1000; x < 1500; ++x) {
            const double base = 236.0 + 0.04 * (t.at(x, y) - 128.0);
            const double a = std::sin(std::numbers::pi * (x - 1000) / 500.0);
            const size_t k = static_cast<size_t>(y) * C + x;
            s.grey[0][k] = Grey(base);
            s.grey[1][k] = Grey(base + 14.0 * a * a * (1.0 - 0.5 * y / sky));
        }
    gpu.resetSeamFlow();
    insv::SeamFlowField f;
    if (!Estimate(gpu, s, f))
        return;
    auto report = [&](const char* who, const insv::SeamFlowField& g) {
        const double inSky = P90Flow(g, 1040, 1460, 4, sky - 4);
        const double below = P90Flow(g, 1040, 1460, sky, R - 8);
        const double across = MedianError(g, 0, 6.0, 8, R - 8, [](int x) { return x >= 440 && x < 660; });
        std::printf("  %sbright sky: p90 flow %.3f px in it, %.3f px in the scene below; across the seam "
                    "elsewhere error %.3f px\n",
                    who, inSky, below, across);
        CHECK(inSky < 0.6);
        CHECK(below < 0.3);
        CHECK(across < 0.5);
    };
    report("", f);
#ifdef ORBIS_SEAM_FLOW
    insv::SeamFlowEstimator est;
    insv::SeamFlowField c;
    CHECK(est.estimate(s, c));
    report("CPU, ", c);
#endif
}

void testFade(render::VulkanFisheyeStitcher& gpu, const Texture& t)
{
    // Lens 1 the same picture, 40 grey levels brighter round x = 1000 (the
    // haze the sun lays over one lens): the fade takes the whole band there
    // and stays narrow away from it; the CPU's from the same strips and the
    // GPU's own field agrees.
    auto none = [](int, int, double& dx, double& dy) { dx = dy = 0.0; };
    insv::SeamFlowStrips s = Strips(t, none);
    const int C = s.grid.cols;
    for (int y = 0; y < s.grid.rows; ++y)
        for (int x = 950; x < 1050; ++x) {
            uint8_t& v = s.grey[1][static_cast<size_t>(y) * C + x];
            v = Grey(v + 40.0);
        }
    gpu.resetSeamFlow();
    insv::SeamFlowField g;
    std::string err;
    insv::StitchGpuParams p;
    p.seamFlowCols = C;
    p.seamFlowRows = s.grid.rows;
    if (!Estimate(gpu, s, g) || !gpu.solveSeamFade(p, err) || !gpu.readSeamFlow(g, err)) {
        std::printf("FAIL  seam fade: %s\n", err.c_str());
        ++g_failures;
        return;
    }
    CHECK(static_cast<int>(g.fade.size()) == C);
    if (static_cast<int>(g.fade.size()) != C)
        return;
    CHECK(g.fade[1000] > 0.95f);
    CHECK(g.fade[2000] < 0.02f && g.fade[300] < 0.02f);
    insv::SeamFadeFrame f;
    insv::MeasureSeamFadeFrame(s, g, f);
    insv::SeamFadeWindow w;
    w.add(std::move(f));
    std::vector<float> cpu;
    const float colour[insv::kSeamDepthBins * 3] = {};
    w.solve(colour, cpu);
    double worst = 0.0;
    for (int x = 0; x < C; ++x)
        worst = std::max(worst, std::fabs(static_cast<double>(cpu[x]) - g.fade[x]));
    std::printf("  fade GPU vs CPU: largest difference %.4f\n", worst);
    CHECK(worst < 0.01);
}

#ifdef ORBIS_SEAM_FLOW
void testAgreesWithCpu(render::VulkanFisheyeStitcher& gpu, const Texture& t)
{
    // A near thing crossing the seam, its shift growing towards the middle
    // of the strip; the CPU's DIS and the GPU's within a few tenths of a
    // pixel of each other where it is.
    auto shift = [](int x, int y, double& dx, double& dy) {
        const double a = std::max(0.0, 1.0 - std::abs(x - 1500) / 200.0);
        const double b = std::max(0.0, 1.0 - std::abs(y - 39) / 50.0);
        dx = 3.0 * a * b;
        dy = 0.8 * a * b;
    };
    const insv::SeamFlowStrips s = Strips(t, shift, 5.0);
    gpu.resetSeamFlow();
    insv::SeamFlowField g, c;
    if (!Estimate(gpu, s, g))
        return;
    insv::SeamFlowEstimator est;
    CHECK(est.estimate(s, c));
    std::vector<double> d;
    for (int y = 8; y < s.grid.rows - 8; ++y)
        for (int x = 1300; x < 1700; ++x) {
            double ga[2], gb[2], ca[2], cb[2];
            FlowPx(g, x, y, ga, gb);
            FlowPx(c, x, y, ca, cb);
            d.push_back(std::hypot(ga[0] - ca[0], ga[1] - ca[1]));
            d.push_back(std::hypot(gb[0] - cb[0], gb[1] - cb[1]));
        }
    std::sort(d.begin(), d.end());
    const double med = d[d.size() / 2], p90 = d[d.size() * 9 / 10];
    std::printf("  GPU vs CPU over the near thing: median %.3f px, p90 %.3f px\n", med, p90);
    CHECK(med < 0.15);
    CHECK(p90 < 0.5);
}
#endif

} // namespace

int main()
{
    render::VulkanFisheyeStitcher gpu;
    std::string err;
    if (!gpu.init(err)) {
        std::printf("SKIP  no Vulkan compute device (%s)\n", err.c_str());
        return 77;
    }
    const Texture t(Grid().cols, 7);
    testWholeShift(gpu, t);
    testLocalShift(gpu, t);
    testShiftAtWrap(gpu, t);
    testWindow(gpu, t);
    testWindowOrder(gpu, t);
    testBrightSky(gpu, t);
    testFade(gpu, t);
#ifdef ORBIS_SEAM_FLOW
    testAgreesWithCpu(gpu, t);
#endif
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures;
}
