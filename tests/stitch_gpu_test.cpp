// The GPU stitch (fisheye_stitch.comp) against the CPU's
// (orbiscore::FisheyeStitcher::renderEquirect) on the same synthetic lens
// pictures, for each lens model: an X5's MEI lenses, and the DJI Avata 360's
// theta-polynomial ones with their tangential pair. A different projection
// moves the pattern and shows as a large difference; the two paths differ
// only by float rounding and the GPU's 8-bit sampling.
// Standalone; the exit code is the failure count, 77 (skipped) without a
// Vulkan device.

#include "VulkanFisheyeStitcher.h"

#include <orbis-core/insta360/Calibration.h>
#include <orbis-core/stitch/FisheyeProjection.h>
#include <orbis-core/Image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

constexpr int kLens = 512; // the synthetic lens pictures' side
constexpr int kEqW = 1024;

// A smooth pattern that differs per lens and per channel, so a lens sampled
// in the wrong place shows.
orbiscore::Image Pattern(int lens)
{
    orbiscore::Image im;
    im.alloc(kLens, kLens);
    for (int y = 0; y < kLens; ++y)
        for (int x = 0; x < kLens; ++x) {
            uint8_t* p = &im.rgb[(static_cast<size_t>(y) * kLens + x) * 3];
            p[0] = static_cast<uint8_t>(128 + 100 * std::sin(x / 9.0 + lens));
            p[1] = static_cast<uint8_t>(128 + 100 * std::sin(y / 11.0 - lens));
            p[2] = static_cast<uint8_t>(128 + 100 * std::sin((x + y) / 15.0));
        }
    return im;
}

// Mean absolute difference per channel between the CPU's equirect and the
// GPU's (RGBA), over the pixels either path covers.
double Compare(const orbiscore::FisheyeStitcher& st, render::VulkanFisheyeStitcher& gpu)
{
    const orbiscore::Image l0 = Pattern(0), l1 = Pattern(1);
    const orbiscore::Image cpu = st.renderEquirect(l0, l1, kEqW);
    std::vector<uint8_t> out;
    std::string err;
    if (!gpu.stitch(l0.rgb.data(), l1.rgb.data(), st.gpuParams(), cpu.w, cpu.h, out, err)) {
        std::printf("FAIL  GPU stitch: %s\n", err.c_str());
        ++g_failures;
        return 1e9;
    }
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < static_cast<size_t>(cpu.w) * cpu.h; ++i)
        for (int c = 0; c < 3; ++c) {
            sum += std::abs(static_cast<int>(cpu.rgb[i * 3 + c]) - static_cast<int>(out[i * 4 + c]));
            ++n;
        }
    return n ? sum / n : 1e9;
}

void testMei(render::VulkanFisheyeStitcher& gpu)
{
    // An X5's two lenses in the 10752 x 5376 side-by-side frame.
    orbiscore::CameraCalibration c;
    for (int i = 0; i < 2; ++i) {
        orbiscore::LensCalibExt e;
        e.xi = 2, e.w = 10752, e.h = 5376, e.focalRefW = 5312;
        e.fx = 4294.03 - 16 * i, e.fy = 4293.86 - 16 * i;
        e.cx = 2703.31 + 5370 * i, e.cy = 2684.03 + 8 * i;
        e.yaw = 0.24 - 0.33 * i, e.pitch = 0.07 - 0.2 * i, e.roll = 89.99 - 1.4 * i;
        e.k[0] = 0.23, e.k[1] = 1.6, e.k[2] = -1.24, e.k[3] = -3.17;
        e.p[0] = -0.0014, e.p[1] = 0.0004;
        e.s[0] = -0.004, e.s[1] = -0.003, e.s[2] = 0.002, e.s[3] = 0.0004;
        c.lensesExt.push_back(e);
    }
    orbiscore::StitchParams insta360;
    for (int i = 0; i < 2; ++i)
        insta360.lensPose[i] = orbiscore::Insta360LensPose(i);
    orbiscore::FisheyeStitcher st;
    CHECK(st.configure(c, kLens, kLens, insta360));
    const double d = Compare(st, gpu);
    std::printf("MEI: mean difference %.3f levels\n", d);
    CHECK(d < 1.0);
}

void testThetaPoly(render::VulkanFisheyeStitcher& gpu)
{
    // The Avata 360's pair (docs/formats/dji-360.md §2.1), each in one half
    // of a side-by-side frame of its 3840 calibration frames, and its pose:
    // the master as it is, the slave a half turn about y with the file's
    // small deviation (as a reader puts the whole pose in the mount).
    orbiscore::CameraCalibration c;
    const double fx[2] = {1039.390, 1035.882}, fy[2] = {1039.506, 1036.047};
    const double cx[2] = {1933.663, 1907.738}, cy[2] = {1912.346, 1924.519};
    const double k[2][4] = {{0.07275, -0.02460, 0.01725, -0.00826},
                            {0.09104, -0.04372, 0.02720, -0.01079}};
    const double p[2][2] = {{0.00022, -0.00184}, {0.00005, 0.00054}};
    for (int i = 0; i < 2; ++i) {
        orbiscore::LensCalibExt e;
        e.fx = fx[i], e.fy = fy[i], e.cx = cx[i] + 3840 * i, e.cy = cy[i];
        std::copy(k[i], k[i] + 4, e.k);
        e.p[0] = p[i][0], e.p[1] = p[i][1];
        e.w = 7680, e.h = 3840, e.focalRefW = 3840;
        c.lensesExt.push_back(e);
    }
    orbiscore::StitchParams params;
    params.lensModel = orbiscore::LensModel::ThetaPoly;
    params.thetaK5[0] = 0.00108, params.thetaK5[1] = 0.00132;
    params.lensPose[0] = orbiscore::LensPose{};
    const orbiscore::Mat3 m = orbiscore::mul(orbiscore::rotY(3.13), orbiscore::rotX(0.004));
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            params.lensPose[1].mount[r * 3 + col] = m.m[r][col];
    params.lensPose[1].rollZeroDeg = 0;
    orbiscore::FisheyeStitcher st;
    CHECK(st.configure(c, kLens, kLens, params));
    const double d = Compare(st, gpu);
    std::printf("theta-polynomial: mean difference %.3f levels\n", d);
    CHECK(d < 1.0);

    // With lens masks (orbiscore::LensMask) as the Avata's body makes them: each
    // lens sees its camera from 92-94 deg off its axis over a third of its
    // picture. Both paths leave it out alike, and it changes the picture.
    orbiscore::StitchParams masked = params;
    for (int i = 0; i < 2; ++i) {
        masked.lensMask[i].clearDeg.assign(180, 0.0);
        for (int s = 0; s < 60; ++s)
            masked.lensMask[i].clearDeg[(110 + s + 15 * i) % 180] = 92.0 + 2.0 * std::sin(s / 9.0);
    }
    orbiscore::FisheyeStitcher maskSt;
    CHECK(maskSt.configure(c, kLens, kLens, masked));
    CHECK(maskSt.gpuParams().maskActive == 1);
    const double dm = Compare(maskSt, gpu);
    std::printf("theta-polynomial, lens masks: mean difference %.3f levels\n", dm);
    CHECK(dm < 1.0);
    {
        const orbiscore::Image a = st.renderEquirect(Pattern(0), Pattern(1), kEqW);
        const orbiscore::Image b = maskSt.renderEquirect(Pattern(0), Pattern(1), kEqW);
        size_t changed = 0;
        for (size_t i = 0; i < a.rgb.size(); ++i)
            changed += a.rgb[i] != b.rgb[i];
        CHECK(changed > a.rgb.size() / 200); // the masked stretch of the seam
    }

    // The same lenses read as MEI land elsewhere: the comparison would see it.
    orbiscore::StitchParams wrong = params;
    wrong.lensModel = orbiscore::LensModel::Mei;
    for (orbiscore::LensCalibExt& e : c.lensesExt)
        e.xi = 1.0;
    orbiscore::FisheyeStitcher meiSt;
    CHECK(meiSt.configure(c, kLens, kLens, wrong));
    const orbiscore::Image cpuA = st.renderEquirect(Pattern(0), Pattern(1), kEqW);
    const orbiscore::Image cpuB = meiSt.renderEquirect(Pattern(0), Pattern(1), kEqW);
    double diff = 0;
    for (size_t i = 0; i < cpuA.rgb.size(); ++i)
        diff += std::abs(static_cast<int>(cpuA.rgb[i]) - static_cast<int>(cpuB.rgb[i]));
    CHECK(diff / cpuA.rgb.size() > 10.0);
}

} // namespace

int main()
{
    render::VulkanFisheyeStitcher gpu;
    std::string err;
    if (!gpu.init(err)) {
        std::printf("SKIP  no Vulkan compute device (%s)\n", err.c_str());
        return 77;
    }
    testMei(gpu);
    testThetaPoly(gpu);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures;
}
