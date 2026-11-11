// lens_calib -- a camera's lens calibration from a clip of the chart, the
// app's calibration (src/calib/) without the user interface: find the
// chart's corners, fit, print the figures the app's result page shows and
// whether it passes, write the calibration as the app stores it.
//
//   lens_calib CLIP --square MM [--guard none|standard|premium]
//              [--corners FILE] [--out FILE.json] [--chart W H OUT.pgm]
//
//   --square MM     the chart's square as it was shown, millimetres
//   --guard         what was over the lenses (default none): recorded, and
//                   the file's model is compared with its profile on top
//   --corners FILE  the detections: read if it exists, else found and
//                   written there (to fit again without the long part)
//   --out FILE      the calibration, JSON, as the app keeps it
//   --chart W H OUT.pgm  write the chart picture for a W x H screen and stop

#include "ChartBoard.h"
#include "ChartDetect.h"
#include "LensFit.h"

#include <ClipMetadata.h>
#include <LensCalibrationJson.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void Usage()
{
    std::fprintf(stderr,
                 "usage: lens_calib CLIP --square MM [--guard none|standard|premium]\n"
                 "                  [--corners FILE] [--out FILE.json]\n"
                 "       lens_calib --chart W H OUT.pgm\n");
}

const char* ProblemText(calib::LensFitProblem p)
{
    switch (p) {
    case calib::LensFitProblem::TooFewFrames: return "too few still frames";
    case calib::LensFitProblem::CoverageLens0: return "lens 0: the chart didn't reach its rim";
    case calib::LensFitProblem::CoverageLens1: return "lens 1: the chart didn't reach its rim";
    case calib::LensFitProblem::Rms: return "the corners don't fit closely enough";
    case calib::LensFitProblem::Transfer: return "the lenses disagree about where the chart is";
    }
    return "?";
}

std::string NowUtc()
{
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 5 && std::strcmp(argv[1], "--chart") == 0) {
        const calib::ChartImage img = calib::MakeChartImage(std::atoi(argv[2]), std::atoi(argv[3]));
        if (img.squarePx == 0) {
            std::fprintf(stderr, "screen too small\n");
            return 1;
        }
        std::ofstream o(argv[4], std::ios::binary);
        o << "P5\n" << img.width << ' ' << img.height << "\n255\n";
        o.write(reinterpret_cast<const char*>(img.grey.data()), std::streamsize(img.grey.size()));
        std::printf("square %d px\n", img.squarePx);
        return o ? 0 : 1;
    }
    if (argc < 2 || argv[1][0] == '-') {
        Usage();
        return 2;
    }
    const std::string clipPath = argv[1];
    calib::LensFitOptions opt;
    std::string guard = "none", cornersPath, outPath;
    for (int a = 2; a < argc; ++a) {
        const std::string s = argv[a];
        if (s == "--square" && a + 1 < argc)
            opt.squareMm = std::strtod(argv[++a], nullptr);
        else if (s == "--guard" && a + 1 < argc)
            guard = argv[++a];
        else if (s == "--corners" && a + 1 < argc)
            cornersPath = argv[++a];
        else if (s == "--out" && a + 1 < argc)
            outPath = argv[++a];
        else {
            Usage();
            return 2;
        }
    }
    opt.lensGuard = lenscal::GuardFromName(guard);
    if (!(opt.squareMm > 0) || opt.lensGuard < 0) {
        Usage();
        return 2;
    }
    std::string err;
    const auto clip = camera::ReadClipMetadata(clipPath, &err);
    if (!clip) {
        std::fprintf(stderr, "%s: %s\n", clipPath.c_str(), err.c_str());
        return 1;
    }

    using Clock = std::chrono::steady_clock;
    calib::ChartObservations obs;
    if (!cornersPath.empty() && std::filesystem::exists(cornersPath)) {
        if (!calib::ReadChartObservations(cornersPath, obs)) {
            std::fprintf(stderr,
                         "%s: not a corners file of this version (delete it to find the "
                         "corners again)\n",
                         cornersPath.c_str());
            return 1;
        }
        std::printf("corners from %s\n", cornersPath.c_str());
    } else {
        const auto t0 = Clock::now();
        int last = -1;
        const bool ok = calib::DetectChart(clipPath, obs, err, [&](double p) {
            const int pc = int(p * 100);
            if (pc / 10 != last / 10) {
                std::fprintf(stderr, "  detecting %d %%\n", pc);
                last = pc;
            }
        });
        if (!ok) {
            std::fprintf(stderr, "detection: %s\n", err.c_str());
            return 1;
        }
        std::printf("detected in %.0f s\n", std::chrono::duration<double>(Clock::now() - t0).count());
        if (!cornersPath.empty() && !calib::WriteChartObservations(cornersPath, obs))
            std::fprintf(stderr, "can't write %s\n", cornersPath.c_str());
    }
    std::printf("%d frames read; the chart in %zu (lens 0) and %zu (lens 1) of the sampled ones\n",
                obs.framesRead, obs.lens[0].size(), obs.lens[1].size());

    const auto t1 = Clock::now();
    int lastFit = -1;
    const calib::LensFitResult r = calib::FitLensCalibration(*clip, obs, opt, [&](double p) {
        const int pc = int(p * 100);
        if (pc / 10 != lastFit / 10) {
            std::fprintf(stderr, "  fitting %d %%\n", pc);
            lastFit = pc;
        }
    });
    if (!r.ok) {
        std::fprintf(stderr, "fit: %s\n", r.message.c_str());
        return 1;
    }
    const stitch::LensCalibration& c = r.calibration;
    std::printf("fitted in %.0f s\n", std::chrono::duration<double>(Clock::now() - t1).count());
    std::printf("frames used: lens 0 %d, lens 1 %d, both %d\n", r.framesUsed[0], r.framesUsed[1],
                r.framesBoth);
    std::printf("rms: lens 0 %.3f px, lens 1 %.3f px (the file's calibration: %.3f, %.3f)\n",
                c.rmsPx[0], c.rmsPx[1], r.fileRmsPx[0], r.fileRmsPx[1]);
    std::printf("transfer lens 0 -> 1: %.3f deg median (the file's: %.3f)\n", c.transferDeg,
                r.fileTransferDeg);
    std::printf("coverage: lens 0 to %.0f deg, lens 1 to %.0f deg\n", c.coverageDeg[0],
                c.coverageDeg[1]);
    std::printf("against the file's: content at the seam moves lens 0 %.3f (up to %.3f), lens 1 "
                "%.3f (up to %.3f) deg; the lens 0 -> 1 rotation changes %.3f deg\n",
                r.shiftDeg[0], r.shiftMaxDeg[0], r.shiftDeg[1], r.shiftMaxDeg[1],
                r.rotationChangeDeg);
    for (int l = 0; l < 2; ++l) {
        const insv::LensCalibExt& e = c.lenses[l];
        std::printf("lens %d: xi %.3f fx %.4f fy %.4f cx %.4f cy %.4f ypr %.5f %.5f %.5f\n", l, e.xi,
                    e.fx, e.fy, e.cx, e.cy, e.yaw, e.pitch, e.roll);
    }
    const auto problems = calib::CheckLensFit(r);
    if (problems.empty())
        std::printf("PASS\n");
    for (const auto p : problems)
        std::printf("FAIL: %s\n", ProblemText(p));

    if (!outPath.empty()) {
        stitch::LensCalibration out = c;
        out.serial = clip->serial;
        out.lensGuard = lenscal::GuardFromName(guard);
        out.created = NowUtc();
        std::ofstream o(outPath);
        o << lenscal::ToJson(out).dump(2) << '\n';
        if (!o) {
            std::fprintf(stderr, "can't write %s\n", outPath.c_str());
            return 1;
        }
        std::printf("written to %s\n", outPath.c_str());
    }
    return problems.empty() ? 0 : 3;
}
