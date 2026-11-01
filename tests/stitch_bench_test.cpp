// tools/stitch_bench's measurements on synthetic pictures, where the answer
// is known: an identical copy, a rotation and a radial stretch of one lens
// (geometry), a colour gain (PSNR), a band whose offset from the reference
// holds still or changes from frame to frame (alive), a blur (sharpness).
// No GPU, no clip. Exit code is the failure count.

#include "Metrics.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

#define NEAR(a, b, tol)                                                                             \
    do {                                                                                           \
        const double va = (a), vb = (b);                                                           \
        if (!(std::fabs(va - vb) <= (tol))) {                                                      \
            std::printf("FAIL  %s:%d  %s = %.4f, want %.4f +- %.4f\n", __FILE__, __LINE__, #a, va,  \
                        vb, double(tol));                                                          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

constexpr double kDeg = std::numbers::pi / 180.0;

// A texture on the sphere: a sum of plane waves, wavelengths 2.5-12 deg.
struct SphereTexture {
    struct Wave {
        bench::V3 k;
        double phase;
    };
    std::vector<Wave> waves;
    SphereTexture()
    {
        std::mt19937 rng(7);
        std::normal_distribution<double> n;
        std::uniform_real_distribution<double> u(0, 1);
        for (int i = 0; i < 60; ++i) {
            bench::V3 k{n(rng), n(rng), n(rng)};
            const double len = std::sqrt(bench::Dot(k, k));
            const double mag = 2 * std::numbers::pi / ((2.5 + 9.5 * u(rng)) * kDeg);
            waves.push_back(
                {{k.x / len * mag, k.y / len * mag, k.z / len * mag}, 2 * std::numbers::pi * u(rng)});
        }
    }
    double at(const bench::V3& d) const
    {
        double s = 0;
        for (const Wave& w : waves)
            s += std::sin(bench::Dot(w.k, d) + w.phase);
        return 128 + 90 * s / std::sqrt(double(waves.size()) / 2) * 0.5;
    }
};

// An equirect of fn(direction) -> grey, as RGBA.
template <class Fn> cv::Mat Equirect(int W, Fn fn)
{
    const int H = W / 2;
    cv::Mat m(H, W, CV_8UC4);
    cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t v = cv::saturate_cast<uint8_t>(fn(bench::PixelDir(x, y, W, H)));
                m.at<cv::Vec4b>(y, x) = {v, v, v, 255};
            }
    });
    return m;
}

bench::V3 RotY(const bench::V3& d, double deg)
{
    const double c = std::cos(deg * kDeg), s = std::sin(deg * kDeg);
    return {c * d.x + s * d.z, d.y, -s * d.x + c * d.z};
}

// The seam frame the tests use: lens 0 looking at the image centre
// (longitude 0, sphere -z), the seam circle through the poles at +-90.
bench::SeamFrame Frame()
{
    bench::SeamFrame f;
    f.axis = {0, 0, -1};
    f.u = {1, 0, 0};
    f.v = {0, 1, 0};
    f.lensAxis[0] = {0, 0, -1};
    f.lensAxis[1] = {0, 0, 1};
    f.bandDeg = 5;
    return f;
}

void testGeometryAndPsnr()
{
    const SphereTexture tex;
    const bench::SeamFrame f = Frame();
    const int W = 1920;
    const cv::Mat ours = Equirect(W, [&](const bench::V3& d) { return tex.at(d); });
    const cv::Mat elev = bench::ElevationMap(f, W, W / 2);

    // Identical.
    {
        const bench::PsnrResult p = bench::ComparePsnr(ours, ours, elev, f.bandDeg);
        CHECK(p.whole == 99 && p.band == 99);
        const bench::GeometryResult g = bench::MeasureGeometry(ours, ours, f);
        CHECK(g.bodyMed < 0.01 && g.bandMed < 0.01);
        CHECK(std::fabs(g.rotation.y) < 0.002 * kDeg);
    }
    // The reference turned 0.4 deg about the vertical: ours shows the
    // reference's d at RotY(d, 0.4), so the fit finds +0.4 about +y, and
    // refrot's z (up; a mirror of the sphere frame, so +) takes it back off.
    {
        const cv::Mat ref = Equirect(W, [&](const bench::V3& d) { return tex.at(RotY(d, 0.4)); });
        const bench::GeometryResult g = bench::MeasureGeometry(ours, ref, f);
        NEAR(g.rotation.y / kDeg, 0.4, 0.03);
        NEAR(g.rotation.x / kDeg, 0.0, 0.03);
        NEAR(g.rotation.z / kDeg, 0.0, 0.03);
        const double zero[3] = {0, 0, 0};
        double fixed[3];
        bench::RotFitRefrot(g.rotation, zero, fixed);
        NEAR(fixed[0], 0.4, 0.03);
        // And the refrot it suggests lines the reference up.
        const bench::M3 R = bench::RefRotation(fixed[0], fixed[1], fixed[2]);
        const cv::Mat aligned = bench::Remap(ref, bench::EquirectMaps(W, W / 2, W, W / 2, R));
        const bench::GeometryResult g2 = bench::MeasureGeometry(ours, aligned, f);
        CHECK(std::fabs(g2.rotation.y / kDeg) < 0.02);
        const bench::PsnrResult p = bench::ComparePsnr(ours, ref, elev, f.bandDeg);
        const bench::PsnrResult p2 = bench::ComparePsnr(ours, aligned, elev, f.bandDeg);
        CHECK(p2.whole > p.whole + 6);
    }
    // Lens 0 stretched: ours shows content 0.25 deg further from its axis.
    {
        const bench::V3 axis = f.lensAxis[0];
        const cv::Mat stretched = Equirect(W, [&](const bench::V3& d) {
            if (bench::Dot(d, axis) <= 0)
                return tex.at(d);
            // d sits at theta from the axis; show the content from theta - 0.25.
            const double th = std::acos(std::clamp(bench::Dot(d, axis), -1.0, 1.0));
            const double t0 = std::max(0.0, th - 0.25 * kDeg);
            const bench::V3 perp{d.x - axis.x * std::cos(th), d.y - axis.y * std::cos(th),
                                 d.z - axis.z * std::cos(th)};
            const double pl = std::sqrt(bench::Dot(perp, perp));
            if (pl < 1e-9)
                return tex.at(d);
            const bench::V3 s{axis.x * std::cos(t0) + perp.x / pl * std::sin(t0),
                              axis.y * std::cos(t0) + perp.y / pl * std::sin(t0),
                              axis.z * std::cos(t0) + perp.z / pl * std::sin(t0)};
            return tex.at(s);
        });
        const bench::GeometryResult g = bench::MeasureGeometry(stretched, ours, f);
        for (int b = 2; b <= 7; ++b) {
            NEAR(g.radial[0][b], 0.25, 0.04);
            NEAR(g.tangential[0][b], 0.0, 0.04);
            NEAR(g.radial[1][b], 0.0, 0.03);
        }
    }
    // A colour gain is taken out by the gain-matched PSNR.
    {
        cv::Mat brighter;
        ours.convertTo(brighter, CV_8UC4, 1.05);
        const bench::PsnrResult p = bench::ComparePsnr(ours, brighter, elev, f.bandDeg);
        CHECK(p.whole < 35);
        CHECK(p.wholeGain > 45);
        NEAR(p.gain[0], 1.05, 0.005);
    }
}

// A random texture strip, blurred noise.
cv::Mat Noise(int W, int H, int seed, double sigma = 1.5)
{
    cv::Mat n(H, W, CV_32F);
    cv::RNG rng(seed);
    rng.fill(n, cv::RNG::NORMAL, 0, 60);
    cv::GaussianBlur(n, n, cv::Size(), sigma);
    cv::Mat out;
    cv::Mat(n * 3 + 128).convertTo(out, CV_8U);
    return out;
}

// Shifted by (dx, dy) px, wrapping.
cv::Mat Shift(const cv::Mat& m, int dx, int dy)
{
    cv::Mat out;
    const cv::Mat M = (cv::Mat_<double>(2, 3) << 1, 0, dx, 0, 1, dy);
    cv::warpAffine(m, out, M, m.size(), cv::INTER_NEAREST, cv::BORDER_WRAP);
    return out;
}

void testAlive()
{
    // The scene moves 2 px a frame; ours shows it through a band that is
    // off by a fixed 4 px (a stable error: not alive), then by an amount
    // that changes 3 px (0.3 deg) from frame to frame.
    const bench::StripGrid g{10, 30};
    const double band = 5;
    const cv::Mat a = Noise(g.width() + 40, g.height(), 1);
    const int b0 = int((g.halfDeg - band) * g.pxDeg), b1 = int((g.halfDeg + band) * g.pxDeg);
    auto scene = [&](int t) { return a(cv::Rect(2 * t, 0, g.width(), g.height())).clone(); };
    auto ours = [&](int t, int bandShift) {
        cv::Mat o = scene(t);
        Shift(scene(t), bandShift, 0).rowRange(b0, b1).copyTo(o.rowRange(b0, b1));
        return o;
    };
    std::vector<bench::SeamOffsets> still, alive;
    for (int t = 0; t < 3; ++t) {
        still.push_back(bench::MeasureSeamOffsets(scene(t), ours(t, 4), g, band));
        alive.push_back(bench::MeasureSeamOffsets(scene(t), ours(t, t % 2 ? 7 : 4), g, band));
    }
    // The offset itself: 0.4 deg in the band, nothing past it.
    NEAR(still[0].blocks[10][bench::SeamOffsets::Band0][0], 0.4, 0.03);
    NEAR(still[0].blocks[10][bench::SeamOffsets::Control1][0], 0.0, 0.03);
    for (int t = 1; t < 3; ++t) {
        const bench::AliveResult s = bench::CompareSeamOffsets(still[t - 1], still[t]);
        CHECK(s.parts > 100);
        CHECK(s.bandMed < 0.02);
        CHECK(s.controlMed < 0.02);
        const bench::AliveResult m = bench::CompareSeamOffsets(alive[t - 1], alive[t]);
        NEAR(m.bandMed, 0.3, 0.04);
        CHECK(m.controlMed < 0.02);
    }
}

void testSharpness()
{
    const bench::StripGrid g{20, 30};
    const cv::Mat a = Noise(g.width(), g.height(), 3, 1.0);
    cv::Mat soft;
    cv::GaussianBlur(a, soft, cv::Size(), 1.5);
    const bench::GradientEnergy sharp = bench::MeasureGradientEnergy(a, g, 5);
    const bench::GradientEnergy blurred = bench::MeasureGradientEnergy(soft, g, 5);
    CHECK(blurred.band < 0.5 * sharp.band);
    NEAR(sharp.band / sharp.beside, 1.0, 0.1);
}

void testStripMaps()
{
    // A strip sampled from an equirect lands each pixel on its seam
    // position: row e, column phi.
    const bench::SeamFrame f = Frame();
    const bench::StripGrid g{4, 30};
    const bench::Maps m = bench::StripMaps(f, g, 1440, 720, nullptr);
    for (int r : {0, 60, 119})
        for (int c : {0, 333, 1439}) {
            const bench::V3 d = bench::PixelDir(m.x.at<float>(r, c), m.y.at<float>(r, c), 1440, 720);
            NEAR(f.elevationDeg(d), g.rowDeg(r), 0.01);
        }
}

} // namespace

int main()
{
    testStripMaps();
    testGeometryAndPsnr();
    testAlive();
    testSharpness();
    if (g_failures == 0)
        std::printf("stitch_bench_test: all ok\n");
    return g_failures;
}
