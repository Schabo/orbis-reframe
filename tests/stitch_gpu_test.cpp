// The GPU stitch (fisheye_stitch.comp) against the CPU's
// (orbiscore::FisheyeStitcher::renderEquirect) on the same synthetic lens
// pictures, for each lens model: an X5's MEI lenses, and the DJI Avata 360's
// theta-polynomial ones with their tangential pair. A different projection
// moves the pattern and shows as a large difference; the two paths differ
// only by float rounding and the GPU's 8-bit sampling.
// Also: the privacy blur with more regions than the stitch once held, and
// a stitcher adopting a device another thread submits to takes the
// queue's lock for its own setup's submits and its teardown.
// Standalone; the exit code is the failure count, 77 (skipped) without a
// Vulkan device.

#include "VulkanFisheyeStitcher.h"

#include <orbis-core/insta360/Calibration.h>
#include <orbis-core/stitch/FisheyeProjection.h>
#include <orbis-core/Image.h>

#include <orbis-core/Ycbcr.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
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

// An X5's two lenses in the 10752 x 5376 side-by-side frame.
orbiscore::CameraCalibration x5Lenses()
{
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
    return c;
}

orbiscore::StitchParams insta360Params()
{
    orbiscore::StitchParams insta360;
    for (int i = 0; i < 2; ++i)
        insta360.lensPose[i] = orbiscore::Insta360LensPose(i);
    return insta360;
}

void testMei(render::VulkanFisheyeStitcher& gpu)
{
    orbiscore::FisheyeStitcher st;
    CHECK(st.configure(x5Lenses(), kLens, kLens, insta360Params()));
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

// More privacy regions than the 64 the stitch's uniform buffer once held:
// the GPU blurs the 200th as the CPU does, and refuses a frame with one past
// orbiscore::kMaxBlurQuads rather than leaving any out.
void testManyBlurRegions(render::VulkanFisheyeStitcher& gpu)
{
    // Region k of a grid round the sphere: 8 x 4 deg, 20 to a row 18 deg
    // apart, rows 10 deg apart from -60 deg up, none touching another.
    auto quadAt = [](int k) {
        auto dirOf = [](double lonDeg, double latDeg, float out[3]) {
            const double lon = lonDeg * std::numbers::pi / 180.0;
            const double lat = latDeg * std::numbers::pi / 180.0;
            out[0] = static_cast<float>(std::cos(lat) * std::sin(lon));
            out[1] = static_cast<float>(std::sin(lat));
            out[2] = static_cast<float>(-std::cos(lat) * std::cos(lon));
        };
        const double lon = -171.0 + 18.0 * (k % 20), lat = -60.0 + 10.0 * (k / 20);
        orbiscore::BlurQuad q;
        dirOf(lon, lat, q.corner[0]);
        dirOf(lon + 8.0, lat, q.corner[1]);
        dirOf(lon + 8.0, lat - 4.0, q.corner[2]);
        dirOf(lon, lat - 4.0, q.corner[3]);
        return q;
    };
    auto prepared = [&](int from, int to) {
        std::vector<orbiscore::BlurQuad> qs;
        for (int k = from; k < to; ++k)
            qs.push_back(quadAt(k));
        std::vector<orbiscore::BlurQuadFrame> f;
        orbiscore::prepareBlurQuads(qs, f);
        return f;
    };
    orbiscore::FisheyeStitcher st;
    CHECK(st.configure(x5Lenses(), kLens, kLens, insta360Params()));
    // A fine checker, which the blur flattens.
    orbiscore::Image l0, l1;
    for (orbiscore::Image* im : {&l0, &l1}) {
        im->alloc(kLens, kLens);
        for (int y = 0; y < kLens; ++y)
            for (int x = 0; x < kLens; ++x)
                for (int c = 0; c < 3; ++c)
                    im->rgb[(static_cast<size_t>(y) * kLens + x) * 3 + c] =
                        ((x / 3 + y / 3 + c) & 1) ? 220 : 40;
    }
    auto cpu = [&](const std::vector<orbiscore::BlurQuadFrame>& b) {
        orbiscore::Image eq;
        eq.alloc(kEqW, kEqW / 2);
        st.renderEquirect(l0, l1, eq, nullptr, nullptr, b.data(), static_cast<int>(b.size()));
        return eq;
    };
    auto gpuStitch = [&](const std::vector<orbiscore::BlurQuadFrame>& b, std::string& err) {
        orbiscore::StitchGpuParams p = st.gpuParams();
        p.blur = b;
        std::vector<uint8_t> out;
        if (!gpu.stitch(l0.rgb.data(), l1.rgb.data(), p, kEqW, kEqW / 2, out, err))
            out.clear();
        return out;
    };
    constexpr int kMany = 200;
    const orbiscore::Image cpuPlain = cpu({}), cpuLast = cpu(prepared(kMany - 1, kMany));
    const orbiscore::Image cpuAll = cpu(prepared(0, kMany));
    std::string err;
    const std::vector<uint8_t> gpuPlain = gpuStitch({}, err), gpuAll = gpuStitch(prepared(0, kMany), err);
    CHECK(!gpuPlain.empty() && !gpuAll.empty());
    if (gpuPlain.empty() || gpuAll.empty()) {
        std::printf("FAIL  GPU stitch: %s\n", err.c_str());
        return;
    }
    // Where the last region changes the CPU's picture, the GPU's changes too,
    // to the CPU's colours.
    double offCpu = 0, moved = 0, all = 0;
    size_t n = 0;
    for (size_t i = 0; i < static_cast<size_t>(kEqW) * (kEqW / 2); ++i) {
        int lastMoves = 0;
        for (int c = 0; c < 3; ++c)
            lastMoves = std::max(lastMoves, std::abs(cpuLast.rgb[i * 3 + c] - cpuPlain.rgb[i * 3 + c]));
        for (int c = 0; c < 3; ++c)
            all += std::abs(int(cpuAll.rgb[i * 3 + c]) - int(gpuAll[i * 4 + c]));
        if (lastMoves < 8)
            continue;
        ++n;
        for (int c = 0; c < 3; ++c) {
            offCpu += std::abs(int(cpuAll.rgb[i * 3 + c]) - int(gpuAll[i * 4 + c]));
            moved += std::abs(int(gpuPlain[i * 4 + c]) - int(gpuAll[i * 4 + c]));
        }
    }
    all /= 3.0 * kEqW * (kEqW / 2);
    std::printf("blur: %zu px of the 200th region, GPU off the CPU by %.2f there (%.3f overall), "
                "moved by %.1f\n",
                n, n ? offCpu / (3 * n) : 0.0, all, n ? moved / (3 * n) : 0.0);
    CHECK(n > 50);
    CHECK(n && offCpu / (3 * n) < 2.0);
    CHECK(n && moved / (3 * n) > 8.0);
    CHECK(all < 1.0);
    // One past the limit: no picture.
    err.clear();
    CHECK(gpuStitch(prepared(0, orbiscore::kMaxBlurQuads + 1), err).empty());
    CHECK(err.find("privacy") != std::string::npos);
}


// Overlapping regions (orbiscore_test's testOverlappingBlurQuads on the GPU):
// where region A's fading edge crosses region B's interior the picture is
// fully blurred in either order, and the GPU matches the CPU.
void testOverlappingBlurRegions(render::VulkanFisheyeStitcher& gpu)
{
    auto dirOf = [](double lonDeg, double latDeg, float out[3]) {
        const double lon = lonDeg * std::numbers::pi / 180.0;
        const double lat = latDeg * std::numbers::pi / 180.0;
        out[0] = static_cast<float>(std::cos(lat) * std::sin(lon));
        out[1] = static_cast<float>(std::sin(lat));
        out[2] = static_cast<float>(-std::cos(lat) * std::cos(lon));
    };
    auto quad = [&](double lon0, double lon1, double lat0, double lat1) {
        orbiscore::BlurQuad q;
        dirOf(lon0, lat0, q.corner[0]);
        dirOf(lon1, lat0, q.corner[1]);
        dirOf(lon1, lat1, q.corner[2]);
        dirOf(lon0, lat1, q.corner[3]);
        return q;
    };
    const orbiscore::BlurQuad a = quad(-20.0, 20.0, 10.0, -10.0), b = quad(14.0, 40.0, 5.0, -5.0);
    auto prepared = [](const std::vector<orbiscore::BlurQuad>& qs) {
        std::vector<orbiscore::BlurQuadFrame> f;
        orbiscore::prepareBlurQuads(qs, f);
        return f;
    };
    orbiscore::FisheyeStitcher st;
    CHECK(st.configure(x5Lenses(), kLens, kLens, insta360Params()));
    // A 2 px checker: every cell's mean is the same grey.
    orbiscore::Image l0, l1;
    for (orbiscore::Image* im : {&l0, &l1}) {
        im->alloc(kLens, kLens);
        for (int y = 0; y < kLens; ++y)
            for (int x = 0; x < kLens; ++x)
                for (int c = 0; c < 3; ++c)
                    im->rgb[(static_cast<size_t>(y) * kLens + x) * 3 + c] =
                        ((x / 2 + y / 2) & 1) ? 220 : 40;
    }
    auto gpuStitch = [&](const std::vector<orbiscore::BlurQuadFrame>& f) {
        orbiscore::StitchGpuParams p = st.gpuParams();
        p.blur = f;
        std::vector<uint8_t> out;
        std::string err;
        if (!gpu.stitch(l0.rgb.data(), l1.rgb.data(), p, kEqW, kEqW / 2, out, err)) {
            std::printf("FAIL  GPU stitch: %s\n", err.c_str());
            out.clear();
        }
        return out;
    };
    const std::vector<orbiscore::BlurQuadFrame> fa = prepared({a}), fb = prepared({b});
    const std::vector<uint8_t> plain = gpuStitch({}), onlyB = gpuStitch(fb);
    const std::vector<uint8_t> ab = gpuStitch(prepared({a, b})), ba = gpuStitch(prepared({b, a}));
    CHECK(!plain.empty() && !onlyB.empty() && !ab.empty() && !ba.empty());
    if (plain.empty() || onlyB.empty() || ab.empty() || ba.empty())
        return;
    orbiscore::Image cpuAb;
    cpuAb.alloc(kEqW, kEqW / 2);
    const std::vector<orbiscore::BlurQuadFrame> fab = prepared({a, b});
    st.renderEquirect(l0, l1, cpuAb, nullptr, nullptr, fab.data(), static_cast<int>(fab.size()));
    const int W = kEqW, H = kEqW / 2;
    auto weight = [&](const orbiscore::BlurQuadFrame& f, int x, int y) {
        const double lat = (0.5 - (y + 0.5) / H) * std::numbers::pi;
        const double lon = ((x + 0.5) / W - 0.5) * 2.0 * std::numbers::pi;
        const double d[3] = {std::cos(lat) * std::sin(lon), std::sin(lat),
                             -std::cos(lat) * std::cos(lon)};
        double s, t;
        return orbiscore::blurQuadST(f, d, s, t) ? orbiscore::blurQuadWeight(s, t) : 0.0;
    };
    size_t n = 0;
    double offAb = 0, offBa = 0, detailPlain = 0, offCpu = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double wa = weight(fa[0], x, y), wb = weight(fb[0], x, y);
            if (wb < 1.0 || wa <= 0.0 || wa > 0.5)
                continue;
            ++n;
            const size_t i = static_cast<size_t>(y) * W + x;
            for (int c = 0; c < 3; ++c) {
                offAb += std::abs(int(ab[i * 4 + c]) - int(onlyB[i * 4 + c]));
                offBa += std::abs(int(ba[i * 4 + c]) - int(onlyB[i * 4 + c]));
                detailPlain += std::abs(int(plain[i * 4 + c]) - int(onlyB[i * 4 + c]));
                offCpu += std::abs(int(ab[i * 4 + c]) - int(cpuAb.rgb[i * 3 + c]));
            }
        }
    const double k = n ? 1.0 / (3.0 * n) : 0.0;
    std::printf("overlapping blur: %zu px, off B's blur by %.2f (A first) / %.2f (B first), "
                "plain picture %.1f, GPU off the CPU by %.2f\n",
                n, offAb * k, offBa * k, detailPlain * k, offCpu * k);
    CHECK(n > 50);
    CHECK(detailPlain * k > 30.0);
    CHECK(offAb * k < 3.0);
    CHECK(offBa * k < 3.0);
    CHECK(offCpu * k < 2.0);
}

// A device of our own standing in for FFmpeg's: one compute queue, ycbcr
// conversion on. False where there is none (the test is skipped).
struct OwnDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;

    bool create()
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
            return false;
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());
        for (VkPhysicalDevice pd : devs) {
            uint32_t nq = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
            std::vector<VkQueueFamilyProperties> qf(nq);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
            for (uint32_t i = 0; i < nq; ++i)
                if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    phys = pd;
                    family = i;
                    break;
                }
            if (phys)
                break;
        }
        if (!phys)
            return false;
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.timelineSemaphore = VK_TRUE;
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        f11.samplerYcbcrConversion = VK_TRUE;
        f11.pNext = &f12;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f11;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        return vkCreateDevice(phys, &dci, nullptr, &device) == VK_SUCCESS;
    }
    ~OwnDevice()
    {
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};

// The queue is shared with other threads, so every submit and wait of the
// adopting stitcher goes under the lock -- setup's own (the LUT store's
// identity upload) and a teardown's included, not only the stitch's.
void testAdoptLocksQueue()
{
    OwnDevice own;
    if (!own.create()) {
        std::printf("SKIP  testAdoptLocksQueue: no device with ycbcr conversion\n");
        return;
    }
    int locks = 0, unlocks = 0;
    bool held = false, nested = false, strayUnlock = false;
    render::AdoptedQueue q;
    q.lock = [&] {
        nested |= held;
        held = true;
        ++locks;
    };
    q.unlock = [&] {
        strayUnlock |= !held;
        held = false;
        ++unlocks;
    };
    orbiscore::YcbcrCoding coding;
    auto gpu = std::make_unique<render::VulkanFisheyeStitcher>();
    std::string err;
    if (!gpu->initAdopt(own.instance, own.phys, own.device, own.family, 0, q, err,
                        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, coding)) {
        std::printf("SKIP  testAdoptLocksQueue: %s\n", err.c_str());
        return;
    }
    CHECK(locks > 0); // the identity LUT's upload
    CHECK(!held);
    const int afterSetup = locks;
    gpu.reset(); // teardown idles the queue, under the lock too
    CHECK(locks > afterSetup);
    CHECK(locks == unlocks);
    CHECK(!held);
    CHECK(!nested);
    CHECK(!strayUnlock);
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
    testManyBlurRegions(gpu);
    testOverlappingBlurRegions(gpu);
    testAdoptLocksQueue();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures;
}
