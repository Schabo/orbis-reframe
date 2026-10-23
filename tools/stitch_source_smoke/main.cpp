// Smoke test for stitch::NativeStitchSource: opens a real .insv clip (fast — no
// whole-file stitch), then fetches one or more frames by timestamp, dumping
// each as a PPM so the output can be eyeballed. Passing several timestamps that
// land far apart exercises seek + forward decode, not just one GOP.

#include <ClipSeam.h>
#include <NativeStitchSource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

void WritePpm(const stitch::Frame& frame, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << frame.width << " " << frame.height << "\n255\n";
    for (size_t i = 0; i < frame.rgba.size(); i += 4) {
        out.write(reinterpret_cast<const char*>(&frame.rgba[i]), 3); // drop alpha for PPM
    }
}

const char kUsage[] =
    "usage: stitch_source_smoke [--width N] [--lens-guard none|standard|premium]\n"
    "       [--seam Y,P,R[,CU,CV,S]] [--no-depth-seam] [--dump-depth]\n"
    "       [--measure-seam IN_MS OUT_MS]\n"
    "       <input.insv> <timestamp_ms>...\n"
    "   or: stitch_source_smoke [options] --range START END STEP <input.insv>\n";

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    if (argc < 3) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    stitch::PreviewStitchConfig config;
    int64_t range_start = -1, range_end = -1, range_step = 0;
    int64_t seam_in = -1, seam_out = -1;
    bool seam_ok = false;
    insv::SeamCorrection seam;
    bool dump_depth = false;
    int arg = 1;
    for (; arg < argc; ++arg) {
        const std::string a = argv[arg];
        if (a == "--width" && arg + 1 < argc) {
            config.width = std::atoi(argv[++arg]);
            config.height = config.width / 2;
        } else if (a == "--lens-guard" && arg + 1 < argc) {
            const std::string g = argv[++arg];
            config.lensGuard = g == "none"       ? stitch::PreviewStitchConfig::LensGuard::None
                               : g == "standard" ? stitch::PreviewStitchConfig::LensGuard::Standard
                                                 : stitch::PreviewStitchConfig::LensGuard::Premium;
        } else if (a == "--seam" && arg + 1 < argc) {
            // --seam Y,P,R[,CU,CV,S] : a known seam fine-tune -- lens 1 yaw /
            // pitch / roll (deg), its centre and the radial scale (% of the
            // lens width, %), as the app's status line shows it
            double cu = 0, cv = 0, scalePct = 0;
            const int n = std::sscanf(argv[++arg], "%lf,%lf,%lf,%lf,%lf,%lf", &seam.lens1RotDeg[0],
                                      &seam.lens1RotDeg[1], &seam.lens1RotDeg[2], &cu, &cv,
                                      &scalePct);
            seam_ok = n == 3 || n == 6;
            seam.lens1Centre[0] = cu * 0.01;
            seam.lens1Centre[1] = cv * 0.01;
            seam.radialScale = 1.0 + scalePct * 0.01;
        } else if (a == "--no-depth-seam") {
            config.depthSeam = false;
        } else if (a == "--dump-depth") {
            // after each frame: the depth-aware seam's table
            dump_depth = true;
        } else if (a == "--measure-seam" && arg + 2 < argc) {
            // --measure-seam IN OUT : stitch::MeasureClipSeam over [IN, OUT] ms,
            // then stitch with the result.
            seam_in = std::atoll(argv[++arg]);
            seam_out = std::atoll(argv[++arg]);
        } else if (a == "--range" && arg + 3 < argc) {
            // --range START END STEP : SeekTo(START) then ReadFrame() until END,
            // writing a numbered PPM every STEP ms (approx). For eyeballing
            // stabilization over a run of frames from one chunk.
            range_start = std::atoll(argv[++arg]);
            range_end = std::atoll(argv[++arg]);
            range_step = std::atoll(argv[++arg]);
        } else {
            break;
        }
    }
    if (argc - arg < (range_step > 0 ? 1 : 2)) {
        std::cerr << "need <input.insv>" << (range_step > 0 ? "" : " and at least one <timestamp_ms>")
                  << std::endl;
        return 2;
    }
    const std::string input_path = argv[arg++];

    std::unique_ptr<stitch::IStitchSource> source = stitch::MakeStitchSource();
    std::cout << "Opening " << input_path << " (native backend, "
              << config.width << "x" << config.height
              << ", metadata + trailer only, no stitching yet)..." << std::endl;
    if (seam_in >= 0) {
        const auto r = stitch::MeasureClipSeam(input_path, seam_in, seam_out, config.lensGuard);
        std::cout << "Seam: " << (r.ok ? "ok" : "FAILED") << " (" << r.message << ")" << std::endl;
        seam_ok = r.ok;
        seam = r.correction;
    }
    if (!source->Open(input_path, config)) {
        std::cerr << "Open failed: " << source->GetLastError() << std::endl;
        return 1;
    }
    if (seam_ok)
        source->SetSeamCorrection(seam);

    const stitch::ClipInfo& info = source->GetInfo();
    std::cout << "Source: " << info.width << "x" << info.height << " @ " << info.fps << " fps, "
              << info.duration_ms << " ms" << std::endl;

    if (range_step > 0) {
        if (!source->SeekTo(range_start)) {
            std::cerr << "SeekTo(" << range_start << ") failed: " << source->GetLastError() << "\n";
            return 1;
        }
        int n = 0;
        int64_t next_dump = range_start;
        for (;;) {
            const stitch::Frame frame = source->ReadFrame();
            if (frame.rgba.empty())
                break;
            if (frame.timestamp_ms > range_end)
                break;
            if (dump_depth)
                if (auto* ns = dynamic_cast<stitch::NativeStitchSource*>(source.get())) {
                    const float* q = ns->SeamDepthTable();
                    std::printf("depth %lld:", static_cast<long long>(frame.timestamp_ms));
                    for (int b = 0; b < insv::kSeamDepthBins; ++b)
                        std::printf(" %.3f", q[b]);
                    std::printf("\n");
                }
            if (frame.timestamp_ms + 1 >= next_dump) {
                char name[64];
                std::snprintf(name, sizeof(name), "sss_range_%04d.ppm", n++);
                WritePpm(frame, name);
                next_dump += range_step;
            }
        }
        std::cout << "range [" << range_start << "," << range_end << "] -> " << n << " PPMs\n";
        return 0;
    }

    for (int i = arg; i < argc; ++i) {
        const int64_t timestamp_ms = std::atoll(argv[i]);
        const auto start = std::chrono::steady_clock::now();
        const stitch::Frame frame = source->GetFrame(timestamp_ms);
        const double elapsed_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (frame.rgba.empty()) {
            std::cerr << "GetFrame(" << timestamp_ms << ") failed: " << source->GetLastError()
                      << std::endl;
            return 1;
        }
        const std::string output_path = "stitch_source_smoke_" + std::to_string(timestamp_ms) + ".ppm";
        WritePpm(frame, output_path);
        std::cout << "GetFrame(" << timestamp_ms << ") -> " << frame.width << "x" << frame.height
                  << " in " << elapsed_s << "s, wrote " << output_path << std::endl;
        if (dump_depth)
            if (auto* ns = dynamic_cast<stitch::NativeStitchSource*>(source.get())) {
                const float* q = ns->SeamDepthTable();
                std::cout << "seam depth q (1/m) per bin:";
                for (int b = 0; b < insv::kSeamDepthBins; ++b)
                    std::printf("%s%+.2f", b % 16 ? " " : "\n  ", q[b]);
                std::cout << std::endl;
            }
    }
    return 0;
}
