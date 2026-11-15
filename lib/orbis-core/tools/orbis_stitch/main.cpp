// orbis_stitch -- turn an Insta360 .insv into an equirect with no Qt or
// Vulkan linked: the CPU stitch of orbis-core on its own.
//
//   orbis_stitch <clip.insv> [--time-ms N] [--eq-width N] [--out eq.ppm]
//               [--lens-profile NAME]   (field-145 profile, e.g. ProtectorA;
//                                        default bare)
//
// Flow: orbiscore::ReadInsvMakerNotes (trailer parser, orbis-core) ->
// orbiscore::ParseExtendedCalibrationString -> orbiscore::InsvDemux (FFmpeg) x2 lenses ->
// orbiscore::FisheyeStitcher -> PPM.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <orbis-core/insta360/Calibration.h>
#include <orbis-core/stitch/FisheyeProjection.h>
#include <orbis-core/Image.h>
#include <orbis-core/media/InsvDemux.h>
#include <orbis-core/insta360/InsvModels.h>
#include <orbis-core/insta360/InsvTrailer.h> // orbiscore::ReadInsvMakerNotes
#include <orbis-core/stitch/SeamAlignment.h>
#include <orbis-core/stitch/SeamCut.h>
#include <orbis-core/stitch/SeamDepth.h>
#include <orbis-core/stitch/SeamWindow.h>

#include <cmath>

namespace {

[[noreturn]] void die(const std::string& what)
{
    std::fprintf(stderr, "error: %s\n", what.c_str());
    std::exit(1);
}

bool writePpm(const std::string& path, const orbiscore::Image& im)
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
    "usage: orbis_stitch <clip.insv> [--time-ms N] [--eq-width N] [--out eq.ppm]\n"
    "       [--lens-profile NAME]  (e.g. ProtectorA; default bare)\n"
    "       [--lens1 FILE]  (lens 1's own file, e.g. a ONE X2's VID_..._10_...)\n"
    "       [--seam Y,P,R[,CU,CV,S]]  (seam correction: lens 1 yaw / pitch / roll,\n"
    "          deg; its centre offset, % of the lens width; radial scale, %)\n"
    "       [--measure-seam IN_MS OUT_MS]  (fit the seam correction on frames\n"
    "          spread over the range)\n"
    "       [--seam-offset IN_MS OUT_MS]  (the lenses' offset at --seam, deg)\n"
    "       [--seam-frames N]  (frames for both; default 8)\n"
    "       [--lens-colour IN_MS OUT_MS]  (measure the lens colour on 24 frames\n"
    "          spread over the range, at --seam, and stitch with it)\n"
    "       [--colour R,G,B[,SR,SG,SB]]  (stitch with this lens colour: lens 0\n"
    "          against lens 1, %; its slope across the band, %/deg)\n"
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
    std::string outPath = "orbis_stitch.ppm";
    std::string lensProfile;
    std::string lens1File; // empty: both lenses are streams of `clip`
    double seamArg[6] = {0, 0, 0, 0, 0, 0}; // --seam: deg, deg, deg, %, %, %
    int64_t seamIn = -1, seamOut = -1, offsetIn = -1, offsetOut = -1;
    int seamFrames = 8;
    int64_t colourIn = -1, colourOut = -1;
    double colourArg[6] = {0, 0, 0, 0, 0, 0}; // --colour: %, %, %, %/deg, %/deg, %/deg
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
        } else if (k == "--lens-colour") { // IN_MS OUT_MS: measure, then stitch with it
            colourIn = std::atoll(next().c_str());
            colourOut = std::atoll(next().c_str());
        } else if (k == "--colour") {
            const std::string v = next();
            const int n =
                std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &colourArg[0], &colourArg[1],
                            &colourArg[2], &colourArg[3], &colourArg[4], &colourArg[5]);
            if (n != 3 && n != 6)
                die("--colour wants r,g,b[,slope r,g,b]");
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

    const auto rawCalib = orbiscore::ReadInsvMakerNotes(clip);
    if (!rawCalib)
        die("no readable calibration trailer in " + clip);
    std::printf("model=%s fw=%s\n", rawCalib->model.c_str(), rawCalib->firmware.c_str());
    std::printf("Parameters: %s\n", rawCalib->parameters.c_str());

    // Extended MEI calibration (Insta360 Studio's model).
    orbiscore::InsvCalibration calib;
    const auto ext = orbiscore::ReadInsvExtendedCalibrationString(clip);
    // The 56-token string, else the 40-token offset_v3 (the ONE X2's).
    if (!(ext && orbiscore::ParseExtendedCalibrationString(*ext, calib)) &&
        !orbiscore::ParseCalibrationV3String(rawCalib->offset_v3, calib))
        die("no MEI lens calibration in the trailer");
    // fx / fy are in field 27's sensor window; the model's when it's absent.
    const double focalRefW = rawCalib->crop_dst_w > 0 ? rawCalib->crop_dst_w
                                                      : orbiscore::FindInsvModel(rawCalib->model).focalRefW;
    for (orbiscore::LensCalibExt& e : calib.lensesExt)
        e.focalRefW = focalRefW;
    std::printf("extended MEI calibration: xi=%.3f fx=%.1f (lens 0), focal reference %g%s\n",
                calib.lensesExt[0].xi, calib.lensesExt[0].fx, focalRefW,
                rawCalib->crop_dst_w > 0 ? "" : " (not in the file: the model's)");

    // How the camera holds its lenses: the model's (InsvModel::mountDeg).
    const orbiscore::InsvModel& model = orbiscore::FindInsvModel(rawCalib->model);
    const auto mount = [&model](orbiscore::StitchParams& p) {
        p.outYawDeg = model.mountDeg[0];
        p.outPitchDeg = model.mountDeg[1];
        p.outRollDeg = model.mountDeg[2];
    };

    // The lens-guard / dive-case profile by its field-145 name; "" = bare.
    const orbiscore::LensProfile* bareProfile = orbiscore::FindLensProfile(*rawCalib, "bare");
    const orbiscore::LensProfile* accProfile =
        lensProfile.empty() || lensProfile == "bare" ? nullptr
                                                     : orbiscore::FindLensProfile(*rawCalib, lensProfile);
    const bool profileMissing = !accProfile && !lensProfile.empty() && lensProfile != "bare";

    orbiscore::InsvDemux demux;
    std::string err;
    if (!(lens1File.empty() ? demux.open(clip, err)
                            : demux.open({{clip, 0}, {lens1File, 0}}, err)))
        die(err);
    if (demux.lensCount() < 2)
        die("expected >= 2 video streams, found " + std::to_string(demux.lensCount()));

    // --seam in the stitch's units: fractions instead of percentages.
    const auto seamCorrection = [&] {
        orbiscore::SeamCorrection c;
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
        orbiscore::StitchParams params;
        orbiscore::ApplyLensProfile(params, bareProfile, accProfile);
        mount(params);
        std::vector<orbiscore::SeamFrame> frames;
        for (int k = 0; k < seamFrames; ++k) {
            const int64_t t = orbiscore::SnapToKeyframeMs(
                in + (out - in) * (2 * k + 1) / (2 * seamFrames), demux.lens(0).fps);
            orbiscore::Image a, b;
            if (demux.seekTo(t, err) && demux.readNextPair(a, b, nullptr, err))
                frames.push_back(orbiscore::MakeSeamFrame(a, b));
        }
        const int w = demux.lens(0).width, h = demux.lens(0).height;
        if (fit) {
            const auto r = orbiscore::EstimateSeamAlignment(calib, params, w, h, frames);
            std::printf("seam: %s (%s)\n", r.ok ? "ok" : "FAILED", r.message.c_str());
            return r.ok ? 0 : 1;
        }
        params.seam = seamCorrection();
        const double o = orbiscore::MeasureSeamOffset(calib, params, w, h, frames);
        std::printf("seam offset: %.3f deg over %zu frames\n", o, frames.size());
        return o >= 0 ? 0 : 1;
    }

    orbiscore::Image lens0, lens1;
    if (!demux.readFrameAt(0, timeMs, lens0, err))
        die("lens 0: " + err);
    if (!demux.readFrameAt(1, timeMs, lens1, err))
        die("lens 1: " + err);
    std::printf("lens frames: %dx%d and %dx%d\n", lens0.w, lens0.h, lens1.w, lens1.h);
    if (lens0.w != lens1.w || lens0.h != lens1.h)
        die("lens streams differ in size");

    orbiscore::FisheyeStitcher stitcher;
    orbiscore::StitchParams params;
    mount(params);
    // False without a bare profile too (the ONE X2 has no profile table): a
    // failure only when a guard was asked for.
    if ((!orbiscore::ApplyLensProfile(params, bareProfile, accProfile) && accProfile) ||
        profileMissing)
        die("no \"" + lensProfile + "\" lens profile in the trailer");
    params.seam = seamCorrection();
    std::printf("lens profile: %s (fov %.0f deg)\n", lensProfile.empty() ? "bare" : lensProfile.c_str(),
                params.fovDeg);
    if (!stitcher.configure(calib, lens0.w, lens0.h, params))
        die("stitcher configure failed");
    // --colour in the stitch's units: log ratios, per degree.
    for (int k = 0; k < 3; ++k) {
        params.lensColour.logRatio[k] = std::log1p(colourArg[k] * 0.01);
        params.lensColour.slope[k] = colourArg[3 + k] * 0.01;
    }
    if (colourIn >= 0) {
        // As stitch::MeasureClipLensColour: the lenses raw at the seam, no
        // depth, frames evenly over the range.
        constexpr int kColourFrames = 24;
        std::vector<std::unique_ptr<orbiscore::SeamCutStats>> stats;
        std::vector<const orbiscore::SeamCutStats*> frames;
        for (int k = 0; k < kColourFrames; ++k) {
            const int64_t t = orbiscore::SnapToKeyframeMs(
                colourIn + (colourOut - colourIn) * (2 * k + 1) / (2 * kColourFrames),
                demux.lens(0).fps);
            orbiscore::Image a, b;
            if (!demux.seekTo(t, err) || !demux.readNextPair(a, b, nullptr, err))
                continue;
            stats.push_back(std::make_unique<orbiscore::SeamCutStats>());
            orbiscore::MeasureSeamCutStats(stitcher, a, b, nullptr, *stats.back());
            frames.push_back(stats.back().get());
        }
        const orbiscore::LensColourFit r =
            orbiscore::FitLensColour(frames, stitcher.gpuParams().bandRad);
        std::printf("lens colour: %s (%s)\n", r.ok ? "ok" : "FAILED", r.message.c_str());
        if (!r.ok)
            return 1;
        params.lensColour = r.colour;
    }

    params.depthSeam = depthSeam;
    params.baselineM = baselineM;
    if (!stitcher.configure(calib, lens0.w, lens0.h, params))
        die("stitcher configure failed");
    // One frame, cold, as the app's first frame after a seek: its cut
    // statistics on the table of no frames before it (all far).
    orbiscore::SeamWindow window;
    window.configure(stitcher.baselineM(), stitcher.gpuParams().bandRad);
    window.setLensColour(params.lensColour); // the frame's residual against it
    orbiscore::SeamState state;
    orbiscore::SeamDepthFrame depthData{};
    if (depthSeam) {
        auto costs = std::make_unique<orbiscore::SeamCosts>();
        orbiscore::MeasureSeamCosts(stitcher, lens0, lens1, *costs);
        float q0[orbiscore::kSeamDepthBins];
        window.measureTable(timeMs, q0);
        auto cutStats = std::make_unique<orbiscore::SeamCutStats>();
        orbiscore::MeasureSeamCutStats(stitcher, lens0, lens1, q0, *cutStats);
        window.add(timeMs, *costs, *cutStats);
        window.solve(timeMs, state);
        orbiscore::SeamDepthData(*costs, stitcher.baselineM(), window.tuning.depth, depthData);
        const float* q = state.q;
        std::printf("depth seam: baseline %+.4f m, mean confidence %.2f\n",
                    2 * stitcher.lensCentre(0).x * stitcher.seamAxis().x +
                        2 * stitcher.lensCentre(0).y * stitcher.seamAxis().y +
                        2 * stitcher.lensCentre(0).z * stitcher.seamAxis().z,
                    state.depthConf);
        if (depthDump) {
            int k0 = 0;
            for (int k = 1; k < orbiscore::kSeamDepthCands; ++k)
                if (std::fabs(orbiscore::SeamCandQ(k)) < std::fabs(orbiscore::SeamCandQ(k0)))
                    k0 = k;
            std::printf(" bin  phi   tex   ncc@0  best(q)        uniq  conf  table q  dist\n");
            double sum0 = 0, sumBest = 0, sumTable = 0;
            int nTex = 0;
            for (int b = 0; b < orbiscore::kSeamDepthBins; ++b) {
                int kb = 0;
                for (int k = 1; k < orbiscore::kSeamDepthCands; ++k)
                    if (costs->ncc[b][k] > costs->ncc[b][kb])
                        kb = k;
                if (costs->texture[b][k0] > 6.0) {
                    int kt = 0; // the candidate nearest the solved table
                    for (int k = 1; k < orbiscore::kSeamDepthCands; ++k)
                        if (std::fabs(orbiscore::SeamCandQ(k) - q[b]) < std::fabs(orbiscore::SeamCandQ(kt) - q[b]))
                            kt = k;
                    sum0 += costs->ncc[b][k0];
                    sumBest += costs->ncc[b][kb];
                    sumTable += costs->ncc[b][kt];
                    ++nTex;
                }
                std::printf("%4d %5.1f %5.1f %6.3f %6.3f(%+5.2f)  %5.2f %5.2f  %+6.2f  %s\n", b,
                            (b + 0.5) * 360.0 / orbiscore::kSeamDepthBins, costs->texture[b][k0],
                            costs->ncc[b][k0], costs->ncc[b][kb], orbiscore::SeamCandQ(kb),
                            depthData.unique[b], depthData.conf[b], q[b],
                            q[b] > 0.05 ? (std::to_string(1.0 / q[b]).substr(0, 4) + " m").c_str()
                                        : "far");
            }
            if (nTex)
                std::printf("textured bins %d: mean ncc at infinity %.3f, at the table %.3f, "
                            "at each bin's best q %.3f\n",
                            nTex, sum0 / nTex, sumTable / nTex, sumBest / nTex);
        }
    }

    orbiscore::Image eq = stitcher.renderEquirect(lens0, lens1, eqWidth,
                                                  depthSeam ? state.q : nullptr,
                                                  depthSeam ? &state.blend : nullptr);
    std::printf("rendered equirect %dx%d\n", eq.w, eq.h);

    if (!writePpm(outPath, eq))
        die("could not write " + outPath);
    std::printf("wrote %s\n", outPath.c_str());
    return 0;
}
