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
// usage: view_compare [--at MS] [--out DIR] <clip.insv>
//   --out DIR writes each render as DIR/<pose>_<path>.nv12 (1920x1080 NV12,
//   BT.709 limited) for a look with ffmpeg.

#include <NativeStitchSource.h>

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

// One NV12 render of the frame at `atMs`: fused (eqW is then only the
// measure grid) or via an eqW equirect.
std::vector<uint8_t> renderView(const std::string& path, int64_t atMs, bool fused, int eqW,
                            const float pose[5])
{
    stitch::NativeStitchSource src;
    stitch::PreviewStitchConfig cfg;
    cfg.width = eqW;
    cfg.height = eqW / 2;
    if (!src.Open(path, cfg) || !src.SeekTo(atMs)) {
        std::fprintf(stderr, "open/seek failed: %s\n", src.GetLastError().c_str());
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
    const stitch::FlatFrame f = src.ReadFlatFrame(kW, kH, pose);
    if (f.nv12.empty())
        std::fprintf(stderr, "render failed: %s\n", src.GetLastError().c_str());
    return f.nv12;
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
    "usage: view_compare [--at MS] [--out DIR] <clip.insv>\n"
    "  --at MS    the frame to compare (default 5000)\n"
    "  --out DIR  also write each render as DIR/<pose>_<path>.nv12 (1920x1080)\n";

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
        else
            clip = a;
    }
    if (clip.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    struct Pose {
        const char* name;
        float p[5];
    };
    const float deg = std::numbers::pi_v<float> / 180.0f;
    const Pose poses[] = {
        {"forward75", {0.0f, 0.0f, 0.0f, 75.0f * deg, 0.0f}},
        {"seam_side75", {90.0f * deg, 0.0f, 0.0f, 75.0f * deg, 0.0f}}, // across the lens seam
        {"zoom30", {20.0f * deg, -5.0f * deg, 10.0f * deg, 30.0f * deg, 0.0f}},
        {"up_wide110", {0.0f, 60.0f * deg, 0.0f, 110.0f * deg, 0.0f}},
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
        }
        std::fflush(stdout);
    }
    return 0;
}
