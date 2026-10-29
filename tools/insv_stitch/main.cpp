// insv_stitch -- turn an Insta360 .insv into an equirect with no Qt or
// Vulkan linked: the CPU stitch of src/insv/ on its own.
//
//   insv_stitch <clip.insv> [--time-ms N] [--eq-width N] [--out eq.ppm]
//               [--lens-profile NAME]   (field-145 profile, e.g. ProtectorA;
//                                        default bare)
//
// Flow: insv::ReadInsvMakerNotes (trailer parser, src/insv) ->
// insv::ParseExtendedCalibrationString -> insv::InsvDemux (FFmpeg) x2 lenses ->
// insv::FisheyeStitcher -> PPM.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <Calibration.h>
#include <FisheyeProjection.h>
#include <Image.h>
#include <InsvDemux.h>
#include <InsvModels.h>
#include <InsvTrailer.h> // insv::ReadInsvMakerNotes
#include <SeamAlignment.h>
#include <SeamCut.h>
#include <SeamDepth.h>

#include <cmath>

namespace {

[[noreturn]] void die(const std::string& what)
{
    std::fprintf(stderr, "error: %s\n", what.c_str());
    std::exit(1);
}

bool writePpm(const std::string& path, const insv::Image& im)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
    std::fwrite(im.rgb.data(), 1, im.rgb.size(), f);
    std::fclose(f);
    return true;
}

const char kUsage[] =
    "usage: insv_stitch <clip.insv> [--time-ms N] [--eq-width N] [--out eq.ppm]\n"
    "       [--lens-profile NAME]  (e.g. ProtectorA; default bare)\n"
    "       [--lens1 FILE]  (lens 1's own file, e.g. a ONE X2's VID_..._10_...)\n"
    "       [--seam Y,P,R[,CU,CV,S]]  (seam correction: lens 1 yaw / pitch / roll,\n"
    "          deg; its centre offset, % of the lens width; radial scale, %)\n"
    "       [--measure-seam IN_MS OUT_MS]  (fit the seam correction on frames\n"
    "          spread over the range)\n"
    "       [--seam-offset IN_MS OUT_MS]  (the lenses' offset at --seam, deg)\n"
    "       [--seam-frames N]  (frames for both; default 8)\n"
    "       [--depth-seam] [--baseline M] [--depth-dump]\n"
    "         (depth-aware seam: measure the per-azimuth stitch distance\n"
    "          on this frame and stitch with it; --baseline < 0 flips which\n"
    "          lens is in front; --depth-dump prints the cost volume summary)\n";

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    if (argc < 2) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const std::string clip = argv[1];
    int64_t timeMs = 0;
    int eqWidth = 2048;
    std::string outPath = "insv_stitch.ppm";
    std::string lensProfile;
    std::string lens1File; // empty: both lenses are streams of `clip`
    double seamArg[6] = {0, 0, 0, 0, 0, 0}; // --seam: deg, deg, deg, %, %, %
    int64_t seamIn = -1, seamOut = -1, offsetIn = -1, offsetOut = -1;
    int seamFrames = 8;
    bool depthSeam = false, depthDump = false;
    double baselineM = 0.0;
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                die("missing value for " + k);
            return argv[++i];
        };
        if (k == "--time-ms")
            timeMs = std::atoll(next().c_str());
        else if (k == "--eq-width")
            eqWidth = std::atoi(next().c_str());
        else if (k == "--out")
            outPath = next();
        else if (k == "--lens-profile")
            lensProfile = next();
        else if (k == "--lens1")
            lens1File = next();
        else if (k == "--measure-seam") { // IN_MS OUT_MS: fit the seam correction, then exit
            seamIn = std::atoll(next().c_str());
            seamOut = std::atoll(next().c_str());
        } else if (k == "--seam-offset") { // IN_MS OUT_MS: offset at --seam, then exit
            offsetIn = std::atoll(next().c_str());
            offsetOut = std::atoll(next().c_str());
        } else if (k == "--seam-frames")
            seamFrames = std::max(1, std::atoi(next().c_str()));
        else if (k == "--depth-seam")
            depthSeam = true;
        else if (k == "--depth-dump")
            depthDump = depthSeam = true;
        else if (k == "--baseline")
            baselineM = std::atof(next().c_str());
        else if (k == "--seam") {
            const std::string v = next();
            const int n = std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &seamArg[0], &seamArg[1],
                                      &seamArg[2], &seamArg[3], &seamArg[4], &seamArg[5]);
            if (n != 3 && n != 6)
                die("--seam wants yaw,pitch,roll[,cu,cv,scale]");
        }
        else
            die("unknown option: " + k);
    }
    if (eqWidth <= 0 || eqWidth % 2)
        die("--eq-width must be a positive even number");

    const auto rawCalib = insv::ReadInsvMakerNotes(clip);
    if (!rawCalib)
        die("no readable calibration trailer in " + clip);
    std::printf("model=%s fw=%s\n", rawCalib->model.c_str(), rawCalib->firmware.c_str());
    std::printf("Parameters: %s\n", rawCalib->parameters.c_str());

    // Extended MEI calibration (Insta360 Studio's model).
    insv::InsvCalibration calib;
    const auto ext = insv::ReadInsvExtendedCalibrationString(clip);
    // The 56-token string, else the 40-token offset_v3 (the ONE X2's).
    if (!(ext && insv::ParseExtendedCalibrationString(*ext, calib)) &&
        !insv::ParseCalibrationV3String(rawCalib->offset_v3, calib))
        die("no MEI lens calibration in the trailer");
    // fx / fy are in field 27's sensor window; the model's when it's absent.
    const double focalRefW = rawCalib->crop_dst_w > 0 ? rawCalib->crop_dst_w
                                                      : insv::FindInsvModel(rawCalib->model).focalRefW;
    for (insv::LensCalibExt& e : calib.lensesExt)
        e.focalRefW = focalRefW;
    std::printf("extended MEI calibration: xi=%.3f fx=%.1f (lens 0), focal reference %g%s\n",
                calib.lensesExt[0].xi, calib.lensesExt[0].fx, focalRefW,
                rawCalib->crop_dst_w > 0 ? "" : " (not in the file: the model's)");

    // How the camera holds its lenses: the model's (InsvModel::mountDeg).
    const insv::InsvModel& model = insv::FindInsvModel(rawCalib->model);
    const auto mount = [&model](insv::StitchParams& p) {
        p.outYawDeg = model.mountDeg[0];
        p.outPitchDeg = model.mountDeg[1];
        p.outRollDeg = model.mountDeg[2];
    };

    // The lens-guard / dive-case profile by its field-145 name; "" = bare.
    const insv::LensProfile* bareProfile = insv::FindLensProfile(*rawCalib, "bare");
    const insv::LensProfile* accProfile =
        lensProfile.empty() || lensProfile == "bare" ? nullptr
                                                     : insv::FindLensProfile(*rawCalib, lensProfile);
    const bool profileMissing = !accProfile && !lensProfile.empty() && lensProfile != "bare";

    insv::InsvDemux demux;
    std::string err;
    if (!(lens1File.empty() ? demux.open(clip, err)
                            : demux.open({{clip, 0}, {lens1File, 0}}, err)))
        die(err);
    if (demux.lensCount() < 2)
        die("expected >= 2 video streams, found " + std::to_string(demux.lensCount()));

    // --seam in the stitch's units: fractions instead of percentages.
    const auto seamCorrection = [&] {
        insv::SeamCorrection c;
        for (int k = 0; k < 3; ++k)
            c.lens1RotDeg[k] = seamArg[k];
        c.lens1Centre[0] = seamArg[3] * 0.01;
        c.lens1Centre[1] = seamArg[4] * 0.01;
        c.radialScale = 1.0 + seamArg[5] * 0.01;
        return c;
    };

    if (seamIn >= 0 || offsetIn >= 0) {
        const bool fit = seamIn >= 0;
        const int64_t in = fit ? seamIn : offsetIn, out = fit ? seamOut : offsetOut;
        insv::StitchParams params;
        insv::ApplyLensProfile(params, bareProfile, accProfile);
        mount(params);
        std::vector<insv::SeamFrame> frames;
        for (int k = 0; k < seamFrames; ++k) {
            const int64_t t = insv::SnapToKeyframeMs(
                in + (out - in) * (2 * k + 1) / (2 * seamFrames), demux.lens(0).fps);
            insv::Image a, b;
            if (demux.seekTo(t, err) && demux.readNextPair(a, b, nullptr, err))
                frames.push_back(insv::MakeSeamFrame(a, b));
        }
        const int w = demux.lens(0).width, h = demux.lens(0).height;
        if (fit) {
            const auto r = insv::EstimateSeamAlignment(calib, params, w, h, frames);
            std::printf("seam: %s (%s)\n", r.ok ? "ok" : "FAILED", r.message.c_str());
            return r.ok ? 0 : 1;
        }
        params.seam = seamCorrection();
        const double o = insv::MeasureSeamOffset(calib, params, w, h, frames);
        std::printf("seam offset: %.3f deg over %zu frames\n", o, frames.size());
        return o >= 0 ? 0 : 1;
    }

    insv::Image lens0, lens1;
    if (!demux.readFrameAt(0, timeMs, lens0, err))
        die("lens 0: " + err);
    if (!demux.readFrameAt(1, timeMs, lens1, err))
        die("lens 1: " + err);
    std::printf("lens frames: %dx%d and %dx%d\n", lens0.w, lens0.h, lens1.w, lens1.h);
    if (lens0.w != lens1.w || lens0.h != lens1.h)
        die("lens streams differ in size");

    insv::FisheyeStitcher stitcher;
    insv::StitchParams params;
    mount(params);
    // False without a bare profile too (the ONE X2 has no profile table): a
    // failure only when a guard was asked for.
    if ((!insv::ApplyLensProfile(params, bareProfile, accProfile) && accProfile) || profileMissing)
        die("no \"" + lensProfile + "\" lens profile in the trailer");
    params.seam = seamCorrection();
    std::printf("lens profile: %s (fov %.0f deg)\n", lensProfile.empty() ? "bare" : lensProfile.c_str(),
                params.fovDeg);
    if (!stitcher.configure(calib, lens0.w, lens0.h, params))
        die("stitcher configure failed");

    params.depthSeam = depthSeam;
    params.baselineM = baselineM;
    if (!stitcher.configure(calib, lens0.w, lens0.h, params))
        die("stitcher configure failed");
    insv::SeamDepthTracker depth;
    insv::SeamCutTracker cut;
    if (depthSeam) {
        auto costs = std::make_unique<insv::SeamCosts>();
        insv::MeasureSeamCosts(stitcher, lens0, lens1, *costs);
        depth.update(*costs, timeMs);
        // One frame, so the cut is measured with this frame's table (the
        // app uses the one before).
        auto cutStats = std::make_unique<insv::SeamCutStats>();
        insv::MeasureSeamCutStats(stitcher, lens0, lens1, depth.table(), *cutStats);
        cut.update(*cutStats, timeMs, stitcher.gpuParams().bandRad);
        const float* q = depth.table();
        std::printf("depth seam: baseline %+.4f m, mean confidence %.2f\n",
                    2 * stitcher.lensCentre(0).x * stitcher.seamAxis().x +
                        2 * stitcher.lensCentre(0).y * stitcher.seamAxis().y +
                        2 * stitcher.lensCentre(0).z * stitcher.seamAxis().z,
                    depth.lastConfidence());
        if (depthDump) {
            int k0 = 0;
            for (int k = 1; k < insv::kSeamDepthCands; ++k)
                if (std::fabs(insv::SeamCandQ(k)) < std::fabs(insv::SeamCandQ(k0)))
                    k0 = k;
            std::printf(" bin  phi   tex   ncc@0  best(q)        table q  dist\n");
            double sum0 = 0, sumBest = 0, sumTable = 0;
            int nTex = 0;
            for (int b = 0; b < insv::kSeamDepthBins; ++b) {
                int kb = 0;
                for (int k = 1; k < insv::kSeamDepthCands; ++k)
                    if (costs->ncc[b][k] > costs->ncc[b][kb])
                        kb = k;
                if (costs->texture[b][k0] > 6.0) {
                    int kt = 0; // the candidate nearest the solved table
                    for (int k = 1; k < insv::kSeamDepthCands; ++k)
                        if (std::fabs(insv::SeamCandQ(k) - q[b]) < std::fabs(insv::SeamCandQ(kt) - q[b]))
                            kt = k;
                    sum0 += costs->ncc[b][k0];
                    sumBest += costs->ncc[b][kb];
                    sumTable += costs->ncc[b][kt];
                    ++nTex;
                }
                std::printf("%4d %5.1f %5.1f %6.3f %6.3f(%+5.2f)  %+6.2f  %s\n", b,
                            (b + 0.5) * 360.0 / insv::kSeamDepthBins, costs->texture[b][k0],
                            costs->ncc[b][k0], costs->ncc[b][kb], insv::SeamCandQ(kb), q[b],
                            q[b] > 0.05 ? (std::to_string(1.0 / q[b]).substr(0, 4) + " m").c_str()
                                        : "far");
            }
            if (nTex)
                std::printf("textured bins %d: mean ncc at infinity %.3f, at the table %.3f, "
                            "at each bin's best q %.3f\n",
                            nTex, sum0 / nTex, sumTable / nTex, sumBest / nTex);
        }
    }

    insv::Image eq = stitcher.renderEquirect(lens0, lens1, eqWidth, depthSeam ? depth.table() : nullptr,
                                             depthSeam ? &cut.blend() : nullptr);
    std::printf("rendered equirect %dx%d\n", eq.w, eq.h);

    if (!writePpm(outPath, eq))
        die("could not write " + outPath);
    std::printf("wrote %s\n", outPath.c_str());
    return 0;
}
