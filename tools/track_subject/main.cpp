// Command-line tool for track::SubjectTracker (src/track/; the research spike
// tools/track_spike, renamed 2026-10-05).
//
// Stitches the clip (stitch::NativeStitchSource) from --t0, feeds each
// equirect + its gyro de-rotation (insv::GyroStabilizer, Lean follow) to the
// tracker from a --box seed, and reduces the path to reframe keys
// (track::reduceToKeys) in tools/reframe_export's --keys format (time_ms yaw
// pitch fovY roll; stabilized frame). --tracker compares the hybrid the app
// uses with plain CSRT / VitTrack / NanoTrack; --render bakes a follow-cam
// clip with the gyro de-rotation applied, like the app would; --dump-equirect
// writes the stabilized sphere with a yaw/pitch grid to read the seed off.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <GyroStabilizer.h>     // insv::GyroStabilizer, StabConfig
#include <ClipMetadata.h>       // camera::ReadClipImu
#include <NativeStitchSource.h> // stitch::NativeStitchSource
#include <ProbeClip.h>          // stitch::ProbeClip
#include <SubjectTracker.h>     // track::SubjectTracker, reduceToKeys

#include "ReframeFlatten.h" // exportpipe::FlattenWriter

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

[[noreturn]] void die(const std::string& what)
{
    std::fprintf(stderr, "error: %s\n", what.c_str());
    std::exit(1);
}

struct Args {
    std::string input;
    int64_t t0Ms = 0;
    double durSec = 10.0;
    bool haveBox = false;
    double boxYawDeg = 0, boxPitchDeg = 0, boxWDeg = 5, boxHDeg = 5;
    std::string tracker = "hybrid"; // hybrid (CSRT + VitTrack verifier) | csrt | vit | nano
    std::string modelDir;         // "" = track::defaultModelDir()
    bool stab = true;
    int workW = 3840;             // analysis equirect width (2:1)
    int crop = 512;               // tracking crop, square px
    double fovFactor = 4.0;       // crop FOV = factor * subject angular size
    double minFovDeg = 20.0, maxFovDeg = 120.0;
    double recenterFrac = 0.18;   // re-centre when the box centre is this far off (fraction of crop)
    double lostScore = 0.4;       // tracker score below this = lost
    int verifyCrop = 256;         // hybrid: verifier crop, square px
    double verifyOffFrac = 0.2;   // hybrid: verifier box this far off the crop centre = disagree
    int refLag = 5;               // hybrid: reference = the verified crop this many frames back
    int64_t occludeFromMs = -1, occludeToMs = -1; // simulated occlusion (frame greyed out)
    float smoothStrength = 0.2f;  // lean-follow strength -> smoothing window
    double rdpTolDeg = 0.5;
    double outFovDeg = 70.0;
    std::string keysOut;
    std::string pathOut;          // per-frame raw track (diagnostic)
    std::string debugDir;         // per-frame crop JPGs with the box drawn
    std::string dumpEquirect;     // write the stabilized equirect at t0 (with a yaw/pitch grid), exit
    std::string render;           // follow-cam mp4
    int outW = 1920, outH = 1080;
};

bool parseWxH(const std::string& s, int& w, int& h)
{
    const auto x = s.find('x');
    if (x == std::string::npos)
        return false;
    w = std::atoi(s.substr(0, x).c_str());
    h = std::atoi(s.substr(x + 1).c_str());
    return w > 0 && h > 0;
}

const char kUsage[] =
    "usage: track_subject <clip.insv> [options]\n"
    "  --t0 MS               start (clip time)\n"
    "  --dur S               tracked duration (default 10)\n"
    "  --dump-equirect F.jpg write the stabilized equirect at t0 with a yaw/pitch grid, exit\n"
    "  --box Y,P,W,H         seed: centre yaw/pitch + angular width/height, degrees,\n"
    "                        stabilized frame (read it off --dump-equirect)\n"
    "  --view-box Y,P,F,A,X,Y,W,H  seed from a box drawn in a view (yaw/pitch/fov deg,\n"
    "                        aspect; box as fractions of the view), like the app\n"
    "  --tracker T           hybrid | csrt | vit | nano (default hybrid); hybrid = CSRT\n"
    "                        tracks, VitTrack scores it (track::SubjectTracker)\n"
    "  --model-dir D         where vittrack.onnx / nanotrack_*.onnx are (default: resources/models/vittrack)\n"
    "  --stab on|off         gyro de-rotation before tracking (default on)\n"
    "  --work-width PX       analysis equirect width (default 3840)\n"
    "  --crop PX             tracking crop size (default 512)\n"
    "  --fov-factor F        crop FOV / subject size (default 4)\n"
    "  --ref-lag N           hybrid: verify against the crop N frames back (default 5)\n"
    "  --occlude A,B         grey out the frames between A and B ms (loss test)\n"
    "  --max-fov DEG         widest tracking crop (default 120)\n"
    "  --recenter-frac F     re-centre when off by this fraction (default 0.18; 0 = every frame)\n"
    "  --lost-score S        tracker score below this = lost (default 0.4)\n"
    "  --smooth F            lean-follow strength 0..1 for path smoothing (default 0.2)\n"
    "  --rdp-tol DEG         key reduction tolerance (default 0.5)\n"
    "  --out-fov DEG         FOV written into the keys (default 70)\n"
    "  --keys-out F          reduced keys (reframe_export --keys format)\n"
    "  --path-out F          per-frame raw track\n"
    "  --debug-dir D         per-frame crop JPGs with the box\n"
    "  --render F.mp4        follow-cam clip from the keys (+ gyro de-rotation)\n"
    "  --size WxH            render size (default 1920x1080)\n";

Args parseArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            std::exit(0);
        }
    if (argc < 2) {
        std::fputs(kUsage, stderr);
        std::exit(2);
    }
    Args a;
    a.input = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc)
                die(std::string("missing value for ") + what);
            return argv[++i];
        };
        if (k == "--t0")
            a.t0Ms = std::atoll(next("--t0").c_str());
        else if (k == "--dur")
            a.durSec = std::atof(next("--dur").c_str());
        else if (k == "--box") {
            const std::string v = next("--box");
            if (std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf", &a.boxYawDeg, &a.boxPitchDeg, &a.boxWDeg,
                            &a.boxHDeg) != 4)
                die("bad --box (want yaw,pitch,w,h degrees)");
            a.haveBox = true;
        } else if (k == "--view-box") {
            // A box drawn in a view, like the app: yaw,pitch,fov,aspect (deg)
            // then x,y,w,h as fractions of the view.
            const std::string v = next("--view-box");
            double vy, vp, vf, va, x, y, w, h;
            if (std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", &vy, &vp, &vf, &va, &x, &y,
                            &w, &h) != 8)
                die("bad --view-box (want yaw,pitch,fov,aspect,x,y,w,h)");
            const track::Seed s = track::seedFromViewBox(x, y, w, h, vy * kDegToRad, vp * kDegToRad,
                                                         0.0, vf * kDegToRad, va);
            a.boxYawDeg = s.yaw * kRadToDeg;
            a.boxPitchDeg = s.pitch * kRadToDeg;
            a.boxWDeg = s.width * kRadToDeg;
            a.boxHDeg = s.height * kRadToDeg;
            a.haveBox = true;
        } else if (k == "--tracker")
            a.tracker = next("--tracker");
        else if (k == "--model-dir")
            a.modelDir = next("--model-dir");
        else if (k == "--stab")
            a.stab = next("--stab") != "off";
        else if (k == "--work-width")
            a.workW = std::atoi(next("--work-width").c_str()) & ~1;
        else if (k == "--crop")
            a.crop = std::atoi(next("--crop").c_str());
        else if (k == "--fov-factor")
            a.fovFactor = std::atof(next("--fov-factor").c_str());
        else if (k == "--max-fov")
            a.maxFovDeg = std::atof(next("--max-fov").c_str());
        else if (k == "--ref-lag")
            a.refLag = std::max(1, std::atoi(next("--ref-lag").c_str()));
        else if (k == "--occlude") {
            const std::string v = next("--occlude");
            long long f0, f1;
            if (std::sscanf(v.c_str(), "%lld,%lld", &f0, &f1) != 2)
                die("bad --occlude (want FROM_MS,TO_MS)");
            a.occludeFromMs = f0;
            a.occludeToMs = f1;
        } else if (k == "--recenter-frac")
            a.recenterFrac = std::atof(next("--recenter-frac").c_str());
        else if (k == "--lost-score")
            a.lostScore = std::atof(next("--lost-score").c_str());
        else if (k == "--smooth")
            a.smoothStrength = static_cast<float>(std::atof(next("--smooth").c_str()));
        else if (k == "--rdp-tol")
            a.rdpTolDeg = std::atof(next("--rdp-tol").c_str());
        else if (k == "--out-fov")
            a.outFovDeg = std::atof(next("--out-fov").c_str());
        else if (k == "--keys-out")
            a.keysOut = next("--keys-out");
        else if (k == "--path-out")
            a.pathOut = next("--path-out");
        else if (k == "--debug-dir")
            a.debugDir = next("--debug-dir");
        else if (k == "--dump-equirect")
            a.dumpEquirect = next("--dump-equirect");
        else if (k == "--render")
            a.render = next("--render");
        else if (k == "--size") {
            if (!parseWxH(next("--size"), a.outW, a.outH))
                die("bad --size (want WxH)");
        } else
            die("unknown option " + k);
    }
    if (a.dumpEquirect.empty() && !a.haveBox)
        die("--box is required (or --dump-equirect to find the subject)");
    if (a.tracker != "vit" && a.tracker != "nano" && a.tracker != "csrt" && a.tracker != "hybrid")
        die("--tracker must be vit, nano, csrt or hybrid");
    return a;
}

void gyroMatrix(const insv::GyroStabilizer& g, bool on, int64_t tMs, float m[9])
{
    float mm[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (on && g.ready())
        g.correctionMatrix(tMs / 1000.0, mm);
    std::copy(&mm[0][0], &mm[0][0] + 9, m);
}

// The stabilized equirect at the current frame, with a labelled yaw/pitch grid
// (yaw = -longitude in this frame; see dirFromYawPitch) to read a --box off.
void dumpStabilizedEquirect(const stitch::Frame& eq, const float stab[9], const std::string& path)
{
    const int W = eq.width, H = eq.height;
    cv::Mat src(H, W, CV_8UC4, const_cast<uint8_t*>(eq.rgba.data()));
    cv::Mat mapX(H, W, CV_32F), mapY(H, W, CV_32F);
    for (int y = 0; y < H; ++y) {
        const double lat = (0.5 - (y + 0.5) / H) * kPi;
        for (int x = 0; x < W; ++x) {
            const double lon = ((x + 0.5) / W - 0.5) * 2 * kPi;
            const double qx = std::cos(lat) * std::sin(lon), qy = std::sin(lat),
                         qz = -std::cos(lat) * std::cos(lon);
            const double wx = stab[0] * qx + stab[1] * qy + stab[2] * qz;
            const double wy = stab[3] * qx + stab[4] * qy + stab[5] * qz;
            const double wz = stab[6] * qx + stab[7] * qy + stab[8] * qz;
            const double wl = std::atan2(wx, -wz), wa = std::asin(std::clamp(wy, -1.0, 1.0));
            mapX.at<float>(y, x) = static_cast<float>((wl / (2 * kPi) + 0.5) * W - 0.5);
            mapY.at<float>(y, x) = static_cast<float>((0.5 - wa / kPi) * H - 0.5);
        }
    }
    cv::Mat out;
    cv::remap(src, out, mapX, mapY, cv::INTER_LINEAR, cv::BORDER_WRAP);
    cv::cvtColor(out, out, cv::COLOR_RGBA2BGR);
    const double fs = W / 3840.0;
    for (int d = -180; d <= 180; d += 15) {
        const int x = static_cast<int>(((-d) / 360.0 + 0.5) * W);
        cv::line(out, {x, 0}, {x, H - 1}, d % 45 ? cv::Scalar(0, 200, 255) : cv::Scalar(0, 0, 255), 1);
        cv::putText(out, "y" + std::to_string(d), {x + 3, H / 2 - 6}, cv::FONT_HERSHEY_SIMPLEX,
                    0.6 * fs, {0, 0, 255}, std::max(1, int(2 * fs)));
    }
    for (int d = -75; d <= 75; d += 15) {
        const int y = static_cast<int>((0.5 - d / 180.0) * H);
        cv::line(out, {0, y}, {W - 1, y}, d ? cv::Scalar(0, 200, 255) : cv::Scalar(0, 0, 255), 1);
        cv::putText(out, "p" + std::to_string(d), {W / 2 + 3, y - 4}, cv::FONT_HERSHEY_SIMPLEX,
                    0.6 * fs, {0, 0, 255}, std::max(1, int(2 * fs)));
    }
    if (!cv::imwrite(path, out))
        die("could not write " + path);
}

} // namespace

int main(int argc, char** argv)
{
    const Args a = parseArgs(argc, argv);

    std::string err;
    stitch::ClipProbe probe;
    if (!stitch::ProbeClip(a.input, probe, err))
        die(err);
    const double fps = probe.fps > 0.0 ? probe.fps : 30.0;
    const int64_t t1Ms = std::min<int64_t>(a.t0Ms + static_cast<int64_t>(a.durSec * 1000.0), probe.duration_ms);
    const double halfFrameMs = 500.0 / fps;

    // --- gyro (Lean follow, the app's default mode) --------------------------
    insv::GyroStabilizer gyro;
    if (a.stab) {
        if (const auto imu = camera::ReadClipImu(a.input))
            gyro.build(*imu, insv::StabConfig::preset(insv::StabMode::LeanFollow), 0.0);
    }
    std::printf("gyro: %s\n", gyro.ready() ? "Lean follow de-rotation" : "off");

    stitch::PreviewStitchConfig sc;
    sc.width = a.workW;
    sc.height = a.workW / 2;
    auto src = std::make_unique<stitch::NativeStitchSource>();
    if (!src->Open(a.input, sc) || !src->SeekTo(a.t0Ms))
        die(src->GetLastError());

    auto readFrom = [&](int64_t fromMs) -> stitch::Frame {
        for (;;) {
            stitch::Frame f = src->ReadFrame();
            if (f.rgba.empty()) {
                if (!src->GetLastError().empty())
                    die(src->GetLastError());
                return f;
            }
            if (f.timestamp_ms + halfFrameMs >= fromMs)
                return f;
        }
    };

    if (!a.dumpEquirect.empty()) {
        const stitch::Frame f = readFrom(a.t0Ms);
        if (f.rgba.empty())
            die("no frame at t0");
        float stab[9];
        gyroMatrix(gyro, a.stab, f.timestamp_ms, stab);
        dumpStabilizedEquirect(f, stab, a.dumpEquirect);
        std::printf("wrote stabilized equirect @ %lld ms -> %s\n", (long long)f.timestamp_ms,
                    a.dumpEquirect.c_str());
        return 0;
    }

    // --- tracking pass --------------------------------------------------------
    track::TrackerParams tp;
    tp.kind = a.tracker == "csrt"   ? track::TrackerKind::Csrt
              : a.tracker == "vit"  ? track::TrackerKind::Vit
              : a.tracker == "nano" ? track::TrackerKind::Nano
                                    : track::TrackerKind::Hybrid;
    tp.modelDir = a.modelDir;
    tp.crop = a.crop;
    tp.fovFactor = a.fovFactor;
    tp.minFovRad = a.minFovDeg * kDegToRad;
    tp.maxFovRad = a.maxFovDeg * kDegToRad;
    tp.recenterFrac = a.recenterFrac;
    tp.lostScore = a.lostScore;
    tp.verifyCrop = a.verifyCrop;
    tp.verifyOffFrac = a.verifyOffFrac;
    tp.refLag = a.refLag;
    track::SubjectTracker tracker(tp);
    tracker.setKeepDebug(!a.debugDir.empty());
    track::Seed seed;
    seed.yaw = a.boxYawDeg * kDegToRad;
    seed.pitch = a.boxPitchDeg * kDegToRad;
    seed.width = a.boxWDeg * kDegToRad;
    seed.height = a.boxHDeg * kDegToRad;

    auto writeDebug = [&](int64_t frame, int64_t tMs, double score, bool good, bool reinit) {
        if (a.debugDir.empty() || tracker.debugCrop().empty())
            return;
        cv::Mat dbg(tp.crop, tp.crop, CV_8UC3, const_cast<uint8_t*>(tracker.debugCrop().data()));
        dbg = dbg.clone();
        const int* b = tracker.debugBox();
        cv::rectangle(dbg, cv::Rect(b[0], b[1], b[2], b[3]),
                      good ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255), 2);
        char txt[128];
        std::snprintf(txt, sizeof txt, "%lld ms  score %.2f  fov %.0f%s", (long long)tMs, score,
                      tracker.debugFovRad() * kRadToDeg, reinit ? "  INIT" : "");
        cv::putText(dbg, txt, {8, 22}, cv::FONT_HERSHEY_SIMPLEX, 0.6, {0, 255, 255}, 2);
        char name[64];
        std::snprintf(name, sizeof name, "/%05lld.jpg", (long long)frame);
        cv::imwrite(a.debugDir + name, dbg);
    };

    std::vector<track::TrackSample> path;
    const auto wall0 = std::chrono::steady_clock::now();
    int64_t frames = 0;
    for (;;) {
        stitch::Frame f = readFrom(a.t0Ms);
        if (f.rgba.empty() || f.timestamp_ms > t1Ms)
            break;
        if (f.timestamp_ms >= a.occludeFromMs && f.timestamp_ms <= a.occludeToMs)
            std::fill(f.rgba.begin(), f.rgba.end(), uint8_t(128));
        float stab[9];
        gyroMatrix(gyro, a.stab, f.timestamp_ms, stab);
        track::TrackSample s;
        if (frames == 0) {
            if (!tracker.start(f.rgba.data(), f.width, f.height, stab, seed, f.timestamp_ms, err))
                die(err);
            s = track::seedSample(seed, f.timestamp_ms);
        } else {
            s = tracker.step(f.rgba.data(), f.width, f.height, stab, f.timestamp_ms);
        }
        writeDebug(frames, f.timestamp_ms, s.score, s.good, s.reinit);
        path.push_back(s);
        if ((++frames % 30) == 0) {
            std::printf("\r  track %lld frames  score %.2f", (long long)frames, s.score);
            std::fflush(stdout);
        }
    }
    const double wallSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    if (path.size() < 2)
        die("too few frames tracked");

    // --- report -----------------------------------------------------------------
    int goodN = 0, lostSpans = 0, longestLost = 0, run = 0;
    for (const track::TrackSample& s : path) {
        if (s.good) {
            ++goodN;
            run = 0;
        } else {
            if (run++ == 0)
                ++lostSpans;
            longestLost = std::max(longestLost, run);
        }
    }
    const int n = static_cast<int>(path.size());
    const auto& tm = tracker.timing();
    std::printf("\r=== %s, stab %s, %d frames (%.2f s) ===\n", a.tracker.c_str(), a.stab ? "on" : "off", n,
                (path.back().tMs - path.front().tMs) / 1000.0);
    std::printf("wall        %.1f fps overall; tracker %.2f ms/frame, crop %.2f ms/frame, verify %.2f ms/frame\n",
                n / wallSec, tm.trackerMs / n, tm.cropMs / n, tm.verifyMs / n);
    std::printf("good        %d/%d (%.1f%%), lost spans %d, longest %d frames\n", goodN, n,
                100.0 * goodN / n, lostSpans, longestLost);
    std::printf("re-inits    %d\n", tm.reinits);

    // --- smooth + reduce ---------------------------------------------------------
    track::ReduceParams rp;
    rp.smoothStrength = a.smoothStrength;
    rp.tolRad = a.rdpTolDeg * kDegToRad;
    std::vector<track::PathKey> smooth;
    const std::vector<track::PathKey> keys = track::reduceToKeys(path, fps, rp, &smooth);
    double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
    for (const track::PathKey& k : smooth) {
        lo[0] = std::min(lo[0], k.yaw);
        hi[0] = std::max(hi[0], k.yaw);
        lo[1] = std::min(lo[1], k.pitch);
        hi[1] = std::max(hi[1], k.pitch);
    }
    std::printf("path        yaw %.1f..%.1f deg, pitch %.1f..%.1f deg\n", lo[0] * kRadToDeg,
                hi[0] * kRadToDeg, lo[1] * kRadToDeg, hi[1] * kRadToDeg);
    std::printf("keys        %zu (RDP tol %.2f deg)\n", keys.size(), a.rdpTolDeg);

    if (!a.pathOut.empty()) {
        FILE* fp = std::fopen(a.pathOut.c_str(), "w");
        if (!fp)
            die("could not open --path-out");
        std::fprintf(fp, "# t_ms yaw_deg pitch_deg size_deg score good reinit smooth_yaw smooth_pitch\n");
        for (int i = 0; i < n; ++i)
            std::fprintf(fp, "%lld %.3f %.3f %.3f %.3f %d %d %.3f %.3f\n", (long long)path[i].tMs,
                         path[i].yaw * kRadToDeg, path[i].pitch * kRadToDeg, path[i].size * kRadToDeg,
                         path[i].score, int(path[i].good), int(path[i].reinit),
                         smooth[i].yaw * kRadToDeg, smooth[i].pitch * kRadToDeg);
        std::fclose(fp);
    }
    const double outFov = a.outFovDeg * kDegToRad;
    if (!a.keysOut.empty()) {
        FILE* fk = std::fopen(a.keysOut.c_str(), "w");
        if (!fk)
            die("could not open --keys-out");
        std::fprintf(fk, "# track_subject %s: time_ms yaw_rad pitch_rad fovY_rad roll_rad (stabilized frame)\n",
                     a.tracker.c_str());
        for (const track::PathKey& k : keys)
            std::fprintf(fk, "%lld %.6f %.6f %.6f 0\n", (long long)k.tMs, k.yaw, k.pitch, outFov);
        std::fclose(fk);
        std::printf("wrote keys -> %s\n", a.keysOut.c_str());
    }

    if (a.render.empty())
        return 0;

    // --- follow-cam render from the reduced keys (linear, like ReframeTrack) ----
    auto keyPose = [&](int64_t tMs, float& yaw, float& pitch) {
        if (tMs <= keys.front().tMs) {
            yaw = float(keys.front().yaw);
            pitch = float(keys.front().pitch);
            return;
        }
        for (size_t k = 1; k < keys.size(); ++k) {
            if (tMs <= keys[k].tMs) {
                const double f = double(tMs - keys[k - 1].tMs) / double(keys[k].tMs - keys[k - 1].tMs);
                yaw = float(keys[k - 1].yaw + (keys[k].yaw - keys[k - 1].yaw) * f);
                pitch = float(keys[k - 1].pitch + (keys[k].pitch - keys[k - 1].pitch) * f);
                return;
            }
        }
        yaw = float(keys.back().yaw);
        pitch = float(keys.back().pitch);
    };

    stitch::PreviewStitchConfig rsc;
    rsc.width = std::max(3840, 2 * a.outW);
    rsc.height = rsc.width / 2;
    auto rsrc = std::make_unique<stitch::NativeStitchSource>();
    if (!rsrc->Open(a.input, rsc) || !rsrc->SeekTo(a.t0Ms))
        die(rsrc->GetLastError());
    if (!rsrc->GpuReframeAvailable())
        die("GPU reframe unavailable");
    exportpipe::FlattenConfig fc;
    fc.out_path = a.render;
    fc.out_w = a.outW;
    fc.out_h = a.outH;
    fc.fps = fps;
    fc.h265 = true;
    fc.bitrate_kbps = 20000;
    exportpipe::FlattenWriter writer;
    if (!writer.open(fc, err))
        die(err);
    auto poseAt = [&](int64_t ptsMs, float pose[4], float stab[9]) {
        keyPose(ptsMs, pose[0], pose[1]);
        pose[2] = 0.0f;
        pose[3] = static_cast<float>(outFov);
        gyroMatrix(gyro, a.stab, ptsMs, stab);
    };
    int64_t written = 0;
    for (;;) {
        stitch::FlatFrame ff = rsrc->ReadFlatFrame(a.outW, a.outH, poseAt);
        if (ff.data.empty()) {
            if (!rsrc->GetLastError().empty())
                die(rsrc->GetLastError());
            break;
        }
        if (ff.timestamp_ms + halfFrameMs < a.t0Ms)
            continue;
        if (ff.timestamp_ms > t1Ms)
            break;
        if (!writer.addNv12Frame(ff.data.data(), ff.width, ff.height, err))
            die(err);
        ++written;
    }
    if (writer.finish(err) < 0)
        die(err);
    std::printf("wrote %lld frames -> %s\n", (long long)written, a.render.c_str());
    return 0;
}
