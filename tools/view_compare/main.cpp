// Does the fused fisheye -> view pass (VulkanFisheyeStitcher::renderViewVk)
// show the same picture as the equirect path, and how much sharper is it?
//
// For one frame of a clip and a few poses, renders the flat 1920x1080 view
//   fused      straight from the fisheyes (one resample),
//   eq N       via an N x N/2 GPU equirect + reproject (two resamples) --
//              today's export path, and what playback shows at N,
// and compares each against `eq 7680` (the full 8K sphere, the closest the
// two-resample path gets to the source) on the Y plane: PSNR, plus mean
// gradient magnitude as a sharpness figure (higher = more detail). All paths
// use the same photometric measure grid (7680), so gains are identical.
//
// It also renders the fused view as P010 (the 10-bit export) and checks it
// against the fused NV12: every sample within one 8-bit code, and samples
// with more than 8 bits of detail -- a 10-bit render, not a widened 8-bit
// one. Exits 1 if not.
//
// usage: view_compare [--at MS] [--seam Y,P,R,CU,CV,S] [--out DIR] <clip.insv>
//   --seam stitches with a seam correction (as stitch_source_smoke --seam),
//   so a measured one can be checked on every path.
//   --out DIR writes each render as DIR/<pose>_<path>.nv12 (1920x1080 NV12,
//   BT.709 limited) for a look with ffmpeg, and the P010 one as
//   DIR/<pose>_fused.p010.

#include <NativeStitchSource.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

constexpr int kW = 1920;
constexpr int kH = 1080;
constexpr int kRefEq = 7680;

insv::SeamCorrection g_seam; // --seam

// One NV12 render of the frame at `atMs`: fused (eqW is then only the
// measure grid) or via an eqW equirect. `p010`: the fused render as P010.
std::vector<uint8_t> renderView(const std::string& path, int64_t atMs, bool fused, int eqW,
                                const view::Pose& pose, bool p010 = false)
{
    stitch::NativeStitchSource src;
    stitch::PreviewStitchConfig cfg;
    cfg.width = eqW;
    cfg.height = eqW / 2;
    if (!src.Open(path, cfg)) {
        std::fprintf(stderr, "open failed: %s\n", src.GetLastError().c_str());
        return {};
    }
    src.SetSeamCorrection(g_seam);
    if (!src.SeekTo(atMs)) {
        std::fprintf(stderr, "seek failed: %s\n", src.GetLastError().c_str());
        return {};
    }
    if (fused)
        unsetenv("INSV_FLAT_VIA_EQUIRECT");
    else
        setenv("INSV_FLAT_VIA_EQUIRECT", "1", 1);
    if (fused && !src.GpuViewAvailable()) {
        std::fprintf(stderr, "fused view path unavailable\n");
        return {};
    }
    src.SetFlatTenBit(p010);
    const stitch::FlatFrame f = src.ReadFlatFrame(kW, kH, &pose);
    if (f.data.empty())
        std::fprintf(stderr, "render failed: %s\n", src.GetLastError().c_str());
    if (f.p010 != p010) {
        std::fprintf(stderr, "render came as %s\n", f.p010 ? "P010" : "NV12");
        return {};
    }
    return f.data;
}

// P010 against NV12 of the same render, over all samples (Y and UV): the
// largest difference in 8-bit codes, and the share of samples whose 10-bit
// code isn't a multiple of 4 (0 for a widened 8-bit picture).
struct DepthCheck {
    int maxDiff8 = 0;
    double fineShare = 0.0;
};
DepthCheck checkP010(const std::vector<uint8_t>& p010, const std::vector<uint8_t>& nv12)
{
    DepthCheck c;
    const size_t n = nv12.size();
    if (p010.size() != 2 * n)
        return {255, 0.0};
    size_t fine = 0;
    for (size_t i = 0; i < n; ++i) {
        const int code10 = (p010[2 * i] | (p010[2 * i + 1] << 8)) >> 6;
        fine += (code10 & 3) != 0;
        // Round half up, as the shader's q8: the nearest 8-bit code.
        c.maxDiff8 = std::max(c.maxDiff8, std::abs((code10 + 2) / 4 - int(nv12[i])));
    }
    c.fineShare = double(fine) / double(n);
    return c;
}

double psnrY(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double se = 0.0;
    for (int i = 0; i < kW * kH; ++i) {
        const double d = double(a[i]) - double(b[i]);
        se += d * d;
    }
    const double mse = se / (kW * kH);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Mean |grad Y| over the interior -- a crude but monotone sharpness figure.
double sharpness(const std::vector<uint8_t>& y)
{
    double sum = 0.0;
    for (int r = 1; r < kH - 1; ++r)
        for (int c = 1; c < kW - 1; ++c) {
            const double gx = double(y[r * kW + c + 1]) - double(y[r * kW + c - 1]);
            const double gy = double(y[(r + 1) * kW + c]) - double(y[(r - 1) * kW + c]);
            sum += std::sqrt(gx * gx + gy * gy);
        }
    return sum / double((kW - 2) * (kH - 2));
}

const char kUsage[] =
    "usage: view_compare [--at MS] [--seam Y,P,R,CU,CV,S] [--out DIR] <clip.insv>\n"
    "  --at MS    the frame to compare (default 5000)\n"
    "  --seam     a seam correction: lens 1 yaw / pitch / roll (deg), its centre\n"
    "             (% of the lens width) and the radial scale (%)\n"
    "  --out DIR  also write each render as DIR/<pose>_<path>.nv12 (1920x1080),\n"
    "             and the P010 one as DIR/<pose>_fused.p010\n";

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    int64_t atMs = 5000;
    std::string outDir;
    std::string clip;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--at" && i + 1 < argc)
            atMs = std::atoll(argv[++i]);
        else if (a == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else if (a == "--seam" && i + 1 < argc) {
            double v[6] = {0, 0, 0, 0, 0, 0};
            if (std::sscanf(argv[++i], "%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4],
                            &v[5]) != 6) {
                std::fputs(kUsage, stderr);
                return 2;
            }
            g_seam = {{v[0], v[1], v[2]}, {v[3] * 0.01, v[4] * 0.01}, 1.0 + v[5] * 0.01};
        }
        else
            clip = a;
    }
    if (clip.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    struct Pose {
        const char* name;
        view::Pose p;
    };
    const float deg = std::numbers::pi_v<float> / 180.0f;
    // A pose by its vertical angle through the centre at distortion d.
    const auto vert = [](float fovYDeg, float d) {
        return view::fovFor<double>(fovYDeg * std::numbers::pi_v<float> / 180.0f, d,
                                    double(kW) / double(kH));
    };
    const Pose poses[] = {
        {"forward75", {0.0f, 0.0f, 0.0f, vert(75.0f, 0.0f), 0.0f}},
        {"seam_side75", {90.0f * deg, 0.0f, 0.0f, vert(75.0f, 0.0f), 0.0f}}, // across the lens seam
        {"zoom30", {20.0f * deg, -5.0f * deg, 10.0f * deg, vert(30.0f, 0.0f), 0.0f}},
        {"up_wide110", {0.0f, 60.0f * deg, 0.0f, vert(110.0f, 0.0f), 0.0f}},
        // The distorted projection (ViewProjection.h): a wide view half way to
        // stereographic, and a tiny planet looking straight down.
        {"wide140_d50", {30.0f * deg, -10.0f * deg, 0.0f, vert(140.0f, 0.5f), 0.5f}},
        {"planet_d100", {0.0f, -90.0f * deg, 0.0f, vert(250.0f, 1.0f), 1.0f}},
    };
    struct Path {
        const char* name;
        bool fused;
        int eqW;
    };
    const Path paths[] = {{"fused", true, kRefEq},
                          {"eq1920", false, 1920},
                          {"eq3840", false, 3840},
                          {"eq5760", false, 5760}};

    std::printf("%s @ %lld ms, %dx%d view; reference = eq %d\n", clip.c_str(),
                static_cast<long long>(atMs), kW, kH, kRefEq);
    bool depthOk = true;
    for (const Pose& pose : poses) {
        const std::vector<uint8_t> ref = renderView(clip, atMs, false, kRefEq, pose.p);
        if (ref.empty())
            return 1;
        std::printf("\n  %-12s  eq%d        sharpness %5.2f\n", pose.name, kRefEq,
                    sharpness(ref));
        auto dump = [&](const char* name, const std::vector<uint8_t>& img) {
            if (outDir.empty())
                return;
            std::ofstream(outDir + "/" + pose.name + "_" + name + ".nv12", std::ios::binary)
                .write(reinterpret_cast<const char*>(img.data()), std::streamsize(img.size()));
        };
        dump("eq7680", ref);
        for (const Path& p : paths) {
            const std::vector<uint8_t> img = renderView(clip, atMs, p.fused, p.eqW, pose.p);
            if (img.empty())
                return 1;
            std::printf("  %-12s  %-11s  sharpness %5.2f  PSNR-Y vs ref %5.1f dB\n", "",
                        p.name, sharpness(img), psnrY(img, ref));
            dump(p.name, img);
            if (!p.fused)
                continue;
            const std::vector<uint8_t> deep = renderView(clip, atMs, true, p.eqW, pose.p, true);
            if (deep.empty())
                return 1;
            const DepthCheck c = checkP010(deep, img);
            // Within a code (rounding) and fine codes in most samples: a
            // widened 8-bit picture would have none.
            const bool ok = c.maxDiff8 <= 1 && c.fineShare > 0.25;
            depthOk &= ok;
            std::printf("  %-12s  %-11s  max diff vs NV12 %d code, %4.1f %% samples finer than "
                        "8-bit  %s\n",
                        "", "fused P010", c.maxDiff8, 100.0 * c.fineShare, ok ? "ok" : "FAIL");
            if (!outDir.empty())
                std::ofstream(outDir + "/" + pose.name + "_fused.p010", std::ios::binary)
                    .write(reinterpret_cast<const char*>(deep.data()),
                           std::streamsize(deep.size()));
        }
        std::fflush(stdout);
    }
    return depthOk ? 0 : 1;
}
