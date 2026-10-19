// Privacy blur from the command line: scan a clip for number plates
// (privacy::PlateScanner, the app's "Scan clip") and/or render stills
// of the blur through the app's stitch paths, to check them by eye.
//
// Options: kUsage below (plate_scan --help).

#include <PlateScanner.h>
#include <StitchFrameSource.h>

#include <GyroStabilizer.h>     // insv::GyroStabilizer, StabConfig
#include <InsvTrailer.h>        // insv::ReadInsvTelemetry
#include <NativeStitchSource.h> // stitch::NativeStitchSource
#include <ProbeClip.h>          // stitch::ProbeClip

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

constexpr double kPi = std::numbers::pi;

[[noreturn]] void die(const std::string& what)
{
    std::fprintf(stderr, "error: %s\n", what.c_str());
    std::exit(1);
}

bool writePpm(const std::string& path, const uint8_t* rgba, int w, int h)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                row[size_t(x) * 3 + c] = rgba[(size_t(y) * w + x) * 4 + c];
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
    return true;
}

const char kUsage[] =
    "usage: plate_scan [options] <clip.insv>\n"
    "  --t0 MS          scan from this source time (default 0)\n"
    "  --dur S          scan this long (default: to the end)\n"
    "  --scan FILE      write the scan (the app's cache format)\n"
    "  --scan-in FILE   load a scan instead of scanning\n"
    "  --cpu            ncnn on the CPU instead of Vulkan\n"
    "  --still MS       (repeatable) stills of the blurred frame at MS, into\n"
    "                   --stills-dir: <ms>_equirect.ppm (3840 GPU equirect),\n"
    "                   <ms>_view.ppm (fused fisheye -> view, 1280x720, aimed at\n"
    "                   the frame's first region unless --view), and with\n"
    "                   --cpu-still <ms>_cpu.ppm (the CPU stitch, slow)\n"
    "  --stills-dir DIR (default .)\n"
    "  --view Y,P,FOV   degrees, for the view still\n"
    "  --no-blur        stills without the blur, for comparison\n"
    "  --follow L,A,W,H \"Blur a plate\" instead of a scan: follow the plate\n"
    "                   in a box centred at lon L / lat A (deg, the stitch's\n"
    "                   sphere frame: lon = atan2(x, -z), lat = asin(y)), W x H\n"
    "                   deg, from --t0 on. --scan writes the result as a\n"
    "                   one-track scan. FOLLOW_PLATE_PX / FOLLOW_CONF /\n"
    "                   FOLLOW_MIN_FOV in the environment override\n"
    "                   privacy::FollowParams.\n"
    "  --back MS        with --follow: follow backwards from --t0 down to\n"
    "                   source time MS instead (ReverseStitchFrameSource);\n"
    "                   BACK_CHUNK in the environment sets its stretch length\n";

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    std::string input, scanOut, scanIn, stillsDir = ".";
    int64_t t0Ms = 0;
    double durSec = 0.0;
    bool cpu = false, cpuStill = false, noBlur = false, haveView = false;
    double view[3] = {0, 0, 60};
    bool follow = false;
    int64_t backMs = -1;
    double followBox[4] = {0, 0, 0, 0};
    std::vector<int64_t> stills;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                die("missing value for " + a);
            return argv[++i];
        };
        if (a == "--t0")
            t0Ms = std::stoll(next());
        else if (a == "--dur")
            durSec = std::stod(next());
        else if (a == "--scan")
            scanOut = next();
        else if (a == "--scan-in")
            scanIn = next();
        else if (a == "--cpu")
            cpu = true;
        else if (a == "--back")
            backMs = std::stoll(next());

        else if (a == "--still")
            stills.push_back(std::stoll(next()));
        else if (a == "--stills-dir")
            stillsDir = next();
        else if (a == "--cpu-still")
            cpuStill = true;
        else if (a == "--no-blur")
            noBlur = true;
        else if (a == "--follow") {
            if (std::sscanf(next().c_str(), "%lf,%lf,%lf,%lf", &followBox[0], &followBox[1], &followBox[2],
                            &followBox[3]) != 4)
                die("--follow L,A,W,H");
            follow = true;
        } else if (a == "--view") {
            if (std::sscanf(next().c_str(), "%lf,%lf,%lf", &view[0], &view[1], &view[2]) != 3)
                die("--view Y,P,FOV");
            haveView = true;
        } else if (!a.empty() && a[0] != '-')
            input = a;
        else
            die("unknown option " + a);
    }
    if (input.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    std::string err;
    stitch::ClipProbe probe;
    if (!stitch::ProbeClip(input, probe, err))
        die(err);
    const double fps = probe.fps > 0.0 ? probe.fps : 30.0;
    const int64_t t1Ms =
        durSec > 0 ? std::min<int64_t>(t0Ms + static_cast<int64_t>(durSec * 1000.0), probe.duration_ms)
                   : probe.duration_ms;

    // Horizon lock levels the detector's tiles; its gyro track also drives the
    // stitch's capture-time correction, as in the app (PlateScanJob), so the
    // scan and the stills stitch the way playback / export do with the
    // stabilization on.
    auto level = std::make_shared<insv::GyroStabilizer>();
    if (const auto tele = insv::ReadInsvTelemetry(input); tele && tele->imu.size() >= 2)
        level->build(*tele, fps, insv::StabConfig::preset(insv::StabMode::HorizonLock), 0.0);
    auto captureTiming = [&level] {
        stitch::CaptureTiming t;
        if (level->ready()) {
            t.readoutSec[0] = t.readoutSec[1] = level->readoutSec();
            t.rotationAt = [level](double tSec, double dtSec, float r[3]) { level->bodyRotation(tSec, dtSec, r); };
        }
        return t;
    };

    privacy::PlateScan scan;
    if (!scanIn.empty()) {
        if (!privacy::loadPlateScan(scanIn, scan, err))
            die(err);
        std::printf("loaded %s: %zu frames, %zu span(s) from %lld ms, %d plates\n", scanIn.c_str(),
                    scan.frames.size(), scan.spans.size(), (long long)scan.frames.front().tMs, scan.plates);
    } else if (follow) {
        privacy::PlateScanner scanner;
        privacy::ScanParams sp;
        sp.gpu = !cpu;
        if (!scanner.init(sp, err))
            die(err);
        stitch::NativeStitchSource src;
        stitch::PreviewStitchConfig sc;
        sc.width = 7680;
        sc.height = 3840;
        if (!src.Open(input, sc) || !src.SeekTo(t0Ms))
            die(src.GetLastError());
        src.SetCaptureTiming(captureTiming());
        const char* chunkEnv = std::getenv("BACK_CHUNK");
        std::unique_ptr<privacy::FrameSource> framesPtr;
        if (backMs >= 0)
            framesPtr = std::make_unique<privacy::ReverseStitchFrameSource>(src, backMs, t0Ms, fps, level,
                                                                            chunkEnv ? std::atoi(chunkEnv) : 0);
        else
            framesPtr = std::make_unique<privacy::StitchFrameSource>(src, t0Ms, t1Ms, fps, level);
        privacy::FrameSource& frames = *framesPtr;
        // The box as a gnomonic rectangle around its centre.
        const double d2r = kPi / 180, lon = followBox[0] * d2r, lat = followBox[1] * d2r;
        const double tx = std::tan(0.5 * followBox[2] * d2r), ty = std::tan(0.5 * followBox[3] * d2r);
        const double fw[3] = {std::cos(lat) * std::sin(lon), std::sin(lat), -std::cos(lat) * std::cos(lon)};
        const double rt[3] = {std::cos(lon), 0.0, std::sin(lon)};
        const double up[3] = {rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2],
                              rt[0] * fw[1] - rt[1] * fw[0]};
        const double sx[4] = {-1, 1, 1, -1}, sy[4] = {1, 1, -1, -1};
        insv::BlurQuad seed;
        for (int k = 0; k < 4; ++k) {
            double v[3], n = 0;
            for (int i = 0; i < 3; ++i) {
                v[i] = fw[i] + rt[i] * tx * sx[k] + up[i] * ty * sy[k];
                n += v[i] * v[i];
            }
            for (int i = 0; i < 3; ++i)
                seed.corner[k][i] = static_cast<float>(v[i] / std::sqrt(n));
        }
        privacy::FollowParams fp;
        if (const char* e = std::getenv("FOLLOW_PLATE_PX"))
            fp.platePx = std::atof(e);
        if (const char* e = std::getenv("FOLLOW_CONF"))
            fp.conf = static_cast<float>(std::atof(e));
        if (const char* e = std::getenv("FOLLOW_MIN_FOV"))
            fp.minTileFovDeg = std::atof(e);
        privacy::FollowResult fr;
        int nDet = 0;
        const auto wall0 = std::chrono::steady_clock::now();
        if (!scanner.follow(frames, seed, fp, nullptr,
                            [&](int64_t tMs, bool det) {
                                nDet += det;
                                std::printf("%c", det ? 'D' : '.');
                                if (fr.frames.size() % 100 == 99)
                                    std::printf(" %lld\n", (long long)tMs);
                                std::fflush(stdout);
                            },
                            fr, err))
            die(err);
        const double wallS = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        std::printf("\nfollowed %zu frames (%.2f s of video) in %.1f s = %.1f fps; %d detected; %s\n",
                    fr.frames.size(),
                    fr.frames.empty() ? 0.0 : std::abs(fr.frames.back().tMs - fr.frames.front().tMs) / 1000.0, wallS,
                    fr.frames.size() / std::max(wallS, 1e-3), fr.detections, fr.lost ? "lost" : "range ended");
        if (backMs >= 0)
            std::reverse(fr.frames.begin(), fr.frames.end()); // into time order
        scan.fps = fps;
        scan.plates = 1;
        scan.frames = fr.frames;
        if (!scan.frames.empty())
            scan.spans.push_back({scan.frames.front().tMs, scan.frames.back().tMs});
        if (!scanOut.empty()) {
            if (!privacy::savePlateScan(scan, scanOut, err))
                die(err);
            std::printf("wrote %s\n", scanOut.c_str());
        }
    } else if (stills.empty() || !scanOut.empty()) {
        std::printf("levelling: %s\n", level->ready() ? "gyro horizon lock" : "none (no gyro)");

        privacy::PlateScanner scanner;
        privacy::ScanParams sp;
        sp.gpu = !cpu;
        if (!scanner.init(sp, err))
            die(err);
        std::printf("detector: %s\n", scanner.device().c_str());

        stitch::NativeStitchSource src;
        stitch::PreviewStitchConfig sc;
        sc.width = 7680; // the photometric measure grid; the CPU fallback's equirect
        sc.height = 3840;
        if (!src.Open(input, sc) || !src.SeekTo(t0Ms))
            die(src.GetLastError());
        src.SetCaptureTiming(captureTiming());
        privacy::StitchFrameSource frames(src, t0Ms, t1Ms, fps, level);
        std::printf("views: %s\n", frames.gpu() ? "GPU, from the lenses" : "CPU, from the equirect");
        const auto wall0 = std::chrono::steady_clock::now();
        int n = 0;
        if (!scanner.run(frames, nullptr,
                         [&](int64_t) {
                             if (++n % 30 == 0) {
                                 const double s = std::chrono::duration<double>(
                                                      std::chrono::steady_clock::now() - wall0)
                                                      .count();
                                 std::printf("\r  %d frames  %.1f fps", n, n / s);
                                 std::fflush(stdout);
                             }
                         },
                         err))
            die(err);
        const double wallS = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        scan = scanner.finish(fps);
        const auto& st = scanner.stats();
        std::printf("\rscanned %d frames in %.1f s = %.1f fps (%.2fx real time)\n", st.frames, wallS,
                    st.frames / wallS, st.frames / wallS / fps);
        std::printf("per frame: render %.1f ms, detect %.1f ms (waited %.1f), quads %.1f ms\n",
                    st.renderMs / st.frames, st.detectMs / st.frames, st.waitMs / st.frames,
                    st.quadMs / st.frames);
        std::printf("detections %d, tracks %d, plates kept %d\n", st.detections, st.tracks, scan.plates);
        int kinds[4] = {0, 0, 0, 0}, framesWith = 0, maxPer = 0;
        for (const auto& fr : scan.frames) {
            for (const auto& r : fr.regions)
                ++kinds[static_cast<int>(r.kind)];
            framesWith += fr.regions.empty() ? 0 : 1;
            maxPer = std::max(maxPer, static_cast<int>(fr.regions.size()));
        }
        std::printf("regions: %d detected, %d filled, %d tracked, %d unverified; on %d/%zu frames, max %d"
                    " per frame\n",
                    kinds[0], kinds[1], kinds[2], kinds[3], framesWith, scan.frames.size(), maxPer);
        if (!scanOut.empty()) {
            if (!privacy::savePlateScan(scan, scanOut, err))
                die(err);
            std::printf("wrote %s\n", scanOut.c_str());
        }
    }

    auto blurFn = [&scan](int64_t ptsMs, std::vector<insv::BlurQuad>& out) { scan.quadsAt(ptsMs, out); };
    for (const int64_t at : stills) {
        // Aim the view at the frame's first region.
        float pose[5] = {float(view[0] * kPi / 180), float(view[1] * kPi / 180), 0.0f, float(view[2] * kPi / 180), 0.0f};
        if (!haveView)
            if (const privacy::ScanFrame* fr = scan.frameAt(at); fr && !fr->regions.empty()) {
                const auto& c = fr->regions.front().quad.corner;
                double d[3] = {0, 0, 0};
                for (int k = 0; k < 4; ++k)
                    for (int i = 0; i < 3; ++i)
                        d[i] += c[k][i];
                const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                // view dir = (-cos p sin y, sin p, -cos p cos y)
                pose[0] = static_cast<float>(-std::atan2(d[0], -d[2]));
                pose[1] = static_cast<float>(std::asin(d[1] / n));
                pose[3] = static_cast<float>(20.0 * kPi / 180);
            }
        const std::string base = stillsDir + "/" + std::to_string(at);
        for (int path = 0; path < 3; ++path) {
            if (path == 2 && !cpuStill)
                continue;
            if (path == 2) {
                setenv("INSV_NO_GPU_DECODE", "1", 1);
                setenv("INSV_NO_GPU_STITCH", "1", 1);
            }
            stitch::NativeStitchSource src;
            stitch::PreviewStitchConfig sc;
            sc.width = 3840;
            sc.height = 1920;
            if (!src.Open(input, sc) || !src.SeekTo(at))
                die(src.GetLastError());
            src.SetCaptureTiming(captureTiming());
            if (!noBlur)
                src.SetBlurQuads(blurFn);
            if (path == 1) {
                if (!src.GpuViewAvailable()) {
                    std::printf("fused view unavailable\n");
                    continue;
                }
                const stitch::Frame f = src.ReadViewFrame(1280, 720, [&](int64_t, float p[5], float[9]) {
                    std::copy(pose, pose + 5, p);
                });
                if (f.rgba.empty())
                    die(src.GetLastError());
                std::printf("view at %lld ms (yaw %.1f pitch %.1f fov %.1f)\n", (long long)f.timestamp_ms,
                            pose[0] * 180 / kPi, pose[1] * 180 / kPi, pose[3] * 180 / kPi);
                writePpm(base + "_view.ppm", f.rgba.data(), f.width, f.height);
            } else {
                const stitch::Frame f = src.ReadFrame();
                if (f.rgba.empty())
                    die(src.GetLastError());
                writePpm(base + (path == 0 ? "_equirect.ppm" : "_cpu.ppm"), f.rgba.data(), f.width, f.height);
            }
            if (path == 2) {
                unsetenv("INSV_NO_GPU_DECODE");
                unsetenv("INSV_NO_GPU_STITCH");
            }
        }
    }
    return 0;
}
