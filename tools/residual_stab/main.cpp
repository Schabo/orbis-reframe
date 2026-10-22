// Residual 2-D stabilization, end to end.
//
// The gyro path (insv::GyroStabilizer) only removes the rotation the .insv IMU
// recorded -- tiny on rigidly-mounted motorcycle footage. This tool runs the
// residual estimator (insv::ResidualPanEstimator): stitch the clip in process
// (stitch::NativeStitchSource), reduce each equirect to grayscale, and measure
// the forward-view 2-D pan that remains after the gyro de-rotation. finish()
// high-passes that into a per-frame yaw/pitch correction to add on top of the
// gyro pose.
//
// --render de-rotate2d bakes gyro + residual into the pose and writes a flat
// clip through the GPU reproject path (exportpipe::FlattenWriter) to eyeball.
//
// --diagnose adds a measure-only stabilization diagnostic: the camera is on a ~1 m stick, not a rigid mount, so the
// shake is a stick swing -- largely *rotational* for far scene content and in
// principle gyro-visible. This mode reports (1) the raw IMU gyro/accel spectra
// (where is the stick resonance? what is the IMU's usable bandwidth?),
// (2) what insv::GyroStabilizer actually removes vs. the raw rotation, sweeping
// the gyro axis-map / scale / time-offset, (3) the image-derived forward-view
// pan spectrum vs. the gyro (does the image show a band the IMU doesn't?), and
// (4) a forward narrow-vs-wide residual split (rotation- vs parallax-dominated).

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include <GyroStabilizer.h>       // insv::GyroStabilizer, StabConfig, MatrixToYawPitchRoll
#include <ClipMetadata.h>         // camera::ReadClipImu
#include <InsvTrailer.h>          // insv::ReadInsvTelemetry (the exposure record)
#include <NativeStitchSource.h>   // stitch::NativeStitchSource, stitch::Frame
#include <ProbeClip.h>            // stitch::ProbeClip
#include <ClipSync.h>             // stitch::MeasureClipSync
#include <ResidualPanEstimator.h> // insv::ResidualPanEstimator, ResidualTrack

#include "ReframeFlatten.h" // exportpipe::FlattenWriter
#include "spectrum.h"       // diag:: FFT + spectral helpers (header-only)

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
    std::string output; // required when --render != off
    int64_t trimInMs = 0;
    int64_t trimOutMs = -1;
    int workW = 1280;                       // analysis equirect width (2:1)
    float residStrength = 0.5f;             // 0..1 -> high-pass window
    std::string stabMode = "leanFollow";    // gyro: off | leanFollow | custom | horizonLock
    float stabStrength = 0.5f;              // Lean-follow Strength for the gyro part
    float directionMs = 0.0f;               // directionLock: the held heading's video time
    float horizonGain = 1.0f;               // accel-fusion gain (horizonLock / any mode)
    float accelLowPassHz = -1.0f;          // LP the accel before Mahony fusion (<0 = keep preset)
    std::string gyroAxes;                   // "" = StabConfig default
    bool stabInSphere = true;               // --stab-compose view: legacy base * corr
    float timeOffsetMs = 0.0f;              // explicit video->IMU offset (0 = derive from trailer)
    float rsStitchMs[2] = {0.0f, 0.0f};     // per-lens row readout in the stitch (0 = off)
    bool rsCols = false;                    // ... scanning fisheye columns instead of rows
    std::string render = "off";             // off | none | derotate | derotate2d
    int outW = 1920, outH = 1080;
    int renderEqW = 0; // 0 => auto max(3840, 2*outW)
    double fovDeg = 90.0;
    double baseYawDeg = 0.0, basePitchDeg = 0.0, baseRollDeg = 0.0; // static pose added to the render
    bool h265 = true;
    int64_t bitrateKbps = 40000;
    bool diagnose = false;           // measure-only stabilization diagnostic
    bool noAutoSync = false;         // don't derive timeOffsetSec from the 0x400 record
    double stickLoHz = 1.0;          // shake / stick-resonance band of interest
    double stickHiHz = 12.0;
    std::string diagCsv;             // optional: write per-frame diagnostic tracks here
    std::string dumpTrack;           // write the raw orientation track (video time) here
    std::string stabIn;              // read per-output-frame 3x3 stab matrices from here
    bool noCaptureTiming = false;    // no per-row capture-time (rolling-shutter) correction
    double lensOffsetMs[2] = {0, 0}; // --lens-offset override (ms)
    bool autoSync = false;           // measure the per-clip sync fine-tune first
    double syncAdjustMs = 0.0;       // explicit fine-tune (ms), added to the derived offset
    bool haveLensOffset = false;
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
    "usage: residual_stab <clip.insv> [out.mp4] [options]\n"
    "  --trim-in MS / --trim-out MS   analysed (and rendered) range\n"
    "  --work-width PX     analysis equirect width, 2:1 (default 1280)\n"
    "  --strength F        residual high-pass strength 0..1 (default 0.5)\n"
    "  --stab-mode M       gyro part: off|leanFollow|custom|horizonLock|directionLock\n"
    "                      (default leanFollow)\n"
    "  --stab-strength F   Lean-follow strength for the gyro part (default 0.5)\n"
    "  --direction-ms MS   directionLock: hold the heading the camera faced at MS (default 0)\n"
    "  --horizon-gain F    accel-fusion gain, 0=pure gyro (default 1)\n"
    "  --accel-lp HZ      low-pass the accel before horizon fusion, Hz "
    "(0=off; unset=preset, horizonLock defaults 1.5)\n"
    "  --stab-compose M    sphere (corr*base, default) | view (legacy base*corr)\n"
    "  --gyro-axes MAP     gyro axis map, e.g. y,z,x (default: StabConfig's)\n"
    "  --time-offset MS    explicit video->IMU offset (default: from the trailer)\n"
    "  --rs-stitch MS[,MS1] per-lens sensor-row readout in the GPU stitch (sign = scan dir)\n"
    "  --rs-cols           ... scanning fisheye columns instead of rows\n"
    "  --auto-sync         measure the clip's sync fine-tune (stitch::MeasureClipSync) and use it\n"
    "  --sync-adjust MS    add MS to the derived video->IMU offset (the fine-tune, by hand)\n"
    "  --no-capture-timing no per-row capture-time (rolling-shutter) correction in the stitch\n"
    "  --lens-offset MS[,MS1] per-lens capture time vs the stab time, ms (overrides the model)\n"
    "  --render MODE       off|none|derotate|derotate2d (default off; needs out.mp4)\n"
    "  --size WxH          render resolution (default 1920x1080)\n"
    "  --render-eq WxH     render equirect intermediate (default max(3840,2*outW))\n"
    "  --fov DEG           render vertical FOV (default 90)\n"
    "  --codec h264|h265   (default h265)   --bitrate KBPS (default 40000)\n"
    "  --diagnose         measure-only stab diagnostic (forces --render off)\n"
    "  --no-auto-sync     don't derive timeOffsetSec from the 0x400 exposure record\n"
    "  --stick-band LO,HI shake band of interest, Hz (default 1,12)\n"
    "  --diag-csv PATH    write per-frame diagnostic tracks to PATH (CSV)\n"
    "  --dump-track PATH  write the raw gyro orientation (t, qw qx qy qz) at 1 kHz\n"
    "                     over the range +-5 s, in video time, then continue\n"
    "  --stab-in PATH     per-output-frame stab matrices (9 floats/line, row-major)\n"
    "                     instead of the gyro's (render only)\n";

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
    int i = 2;
    if (argc >= 3 && argv[2][0] != '-') {
        a.output = argv[2];
        i = 3;
    }
    for (; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                die(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (k == "--trim-in")
            a.trimInMs = std::atoll(next("--trim-in").c_str());
        else if (k == "--trim-out")
            a.trimOutMs = std::atoll(next("--trim-out").c_str());
        else if (k == "--work-width")
            a.workW = std::atoi(next("--work-width").c_str());
        else if (k == "--strength")
            a.residStrength = static_cast<float>(std::atof(next("--strength").c_str()));
        else if (k == "--stab-mode")
            a.stabMode = next("--stab-mode");
        else if (k == "--direction-ms")
            a.directionMs = static_cast<float>(std::atof(next("--direction-ms").c_str()));
        else if (k == "--stab-strength")
            a.stabStrength = static_cast<float>(std::atof(next("--stab-strength").c_str()));
        else if (k == "--horizon-gain")
            a.horizonGain = static_cast<float>(std::atof(next("--horizon-gain").c_str()));
        else if (k == "--accel-lp")
            a.accelLowPassHz = static_cast<float>(std::atof(next("--accel-lp").c_str()));
        else if (k == "--stab-compose")
            a.stabInSphere = next("--stab-compose") == "sphere";
        else if (k == "--gyro-axes")
            a.gyroAxes = next("--gyro-axes");
        else if (k == "--time-offset")
            a.timeOffsetMs = static_cast<float>(std::atof(next("--time-offset").c_str()));
        else if (k == "--rs-stitch") {
            const std::string v = next("--rs-stitch");
            a.rsStitchMs[0] = a.rsStitchMs[1] = static_cast<float>(std::atof(v.c_str()));
            if (const auto c = v.find(','); c != std::string::npos)
                a.rsStitchMs[1] = static_cast<float>(std::atof(v.c_str() + c + 1));
        } else if (k == "--rs-cols")
            a.rsCols = true;
        else if (k == "--render")
            a.render = next("--render");
        else if (k == "--size") {
            if (!parseWxH(next("--size"), a.outW, a.outH))
                die("bad --size");
        } else if (k == "--render-eq") {
            int ew = 0, eh = 0;
            if (!parseWxH(next("--render-eq"), ew, eh) || (ew & 1))
                die("bad --render-eq (want WxH, even width)");
            a.renderEqW = ew;
        } else if (k == "--fov")
            a.fovDeg = std::atof(next("--fov").c_str());
        else if (k == "--yaw")
            a.baseYawDeg = std::atof(next("--yaw").c_str());
        else if (k == "--pitch")
            a.basePitchDeg = std::atof(next("--pitch").c_str());
        else if (k == "--roll")
            a.baseRollDeg = std::atof(next("--roll").c_str());
        else if (k == "--codec")
            a.h265 = (next("--codec") != "h264");
        else if (k == "--bitrate")
            a.bitrateKbps = std::atoll(next("--bitrate").c_str());
        else if (k == "--diagnose")
            a.diagnose = true;
        else if (k == "--no-auto-sync")
            a.noAutoSync = true;
        else if (k == "--stick-band") {
            const std::string v = next("--stick-band");
            const auto comma = v.find(',');
            if (comma == std::string::npos)
                die("bad --stick-band (want LO,HI)");
            a.stickLoHz = std::atof(v.substr(0, comma).c_str());
            a.stickHiHz = std::atof(v.substr(comma + 1).c_str());
            if (!(a.stickHiHz > a.stickLoHz) || a.stickLoHz < 0.0)
                die("bad --stick-band values");
        } else if (k == "--dump-track")
            a.dumpTrack = next("--dump-track");
        else if (k == "--auto-sync")
            a.autoSync = true;
        else if (k == "--sync-adjust")
            a.syncAdjustMs = std::atof(next("--sync-adjust").c_str());
        else if (k == "--no-capture-timing")
            a.noCaptureTiming = true;
        else if (k == "--lens-offset") {
            const std::string v = next("--lens-offset");
            a.lensOffsetMs[0] = a.lensOffsetMs[1] = std::atof(v.c_str());
            if (const auto c = v.find(','); c != std::string::npos)
                a.lensOffsetMs[1] = std::atof(v.c_str() + c + 1);
            a.haveLensOffset = true;
        }
        else if (k == "--stab-in")
            a.stabIn = next("--stab-in");
        else if (k == "--diag-csv")
            a.diagCsv = next("--diag-csv");
        else
            die("unknown option: " + k);
    }
    if (a.diagnose)
        a.render = "off"; // diagnostic is measure-only
    if (a.render != "off" && a.render != "none" && a.render != "derotate" &&
        a.render != "derotate2d")
        die("bad --render (off|none|derotate|derotate2d)");
    if (a.render != "off" && a.output.empty())
        die("--render " + a.render + " needs an out.mp4 argument");
    a.workW &= ~1;
    return a;
}

insv::StabConfig gyroConfig(const Args& a)
{
    insv::StabConfig c;
    if (a.stabMode == "off")
        return c; // mode == Off
    if (a.stabMode == "custom")
        c = insv::StabConfig::preset(insv::StabMode::Custom);
    else if (a.stabMode == "horizonLock")
        c = insv::StabConfig::preset(insv::StabMode::HorizonLock);
    else if (a.stabMode == "directionLock") {
        c = insv::StabConfig::preset(insv::StabMode::DirectionLock);
        c.directionSec = a.directionMs / 1000.0f;
    } else {
        c = insv::StabConfig::preset(insv::StabMode::LeanFollow);
        c.windowYawSec = c.windowPitchSec = c.windowRollSec =
            insv::leanWindowFromStrength(a.stabStrength);
    }
    c.horizonGain = a.horizonGain;
    if (a.accelLowPassHz >= 0.0f) // <0 => keep the mode preset's value
        c.accelLowPassHz = a.accelLowPassHz;
    if (!a.gyroAxes.empty())
        c.gyroAxes = a.gyroAxes;
    c.timeOffsetSec = a.timeOffsetMs / 1000.0f;
    return c;
}

void identity3(float m[3][3])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r][c] = (r == c) ? 1.0f : 0.0f;
}

// interleaved RGBA -> 8-bit luma (BT.601-ish, fast integer).
void rgbaToGray(const uint8_t* rgba, uint8_t* gray, int n)
{
    for (int i = 0; i < n; ++i) {
        const int r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        gray[i] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
    }
}

double rmsFirstDiff(const std::vector<double>& v)
{
    if (v.size() < 2)
        return 0.0;
    double s = 0.0;
    for (size_t i = 1; i < v.size(); ++i) {
        const double d = v[i] - v[i - 1];
        s += d * d;
    }
    return std::sqrt(s / (v.size() - 1));
}

double rms(const std::vector<float>& v)
{
    if (v.empty())
        return 0.0;
    double s = 0.0;
    for (float x : v)
        s += static_cast<double>(x) * x;
    return std::sqrt(s / v.size());
}

float maxAbs(const std::vector<float>& v)
{
    float m = 0.0f;
    for (float x : v)
        m = std::max(m, std::fabs(x));
    return m;
}

// ============================ diagnostic mode ==============================
// Everything below is only reached under --diagnose. Measure-only: no render,
// no model / .orp / shader change.

constexpr double kImuTScale = 1e-6; // trailer timestamp unit (matches GyroStabilizer)

struct SlicedImu {
    std::vector<double> t;                    // seconds, relative to full-track sample 0
    std::vector<std::array<double, 3>> gyro;  // rad/s, raw sensor axes x/y/z
    std::vector<std::array<double, 3>> accel; // g,     raw sensor axes x/y/z
    double srHz = 0.0;                        // sample rate from the median timestep
};

SlicedImu sliceImu(const std::vector<insv::ImuSample>& s, double t0Sec, double t1Sec)
{
    SlicedImu o;
    if (s.size() < 2)
        return o;
    const double ts0 = static_cast<double>(s.front().timestamp);
    std::vector<double> dts;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = (static_cast<double>(s[i].timestamp) - ts0) * kImuTScale;
        if (t < t0Sec || t > t1Sec)
            continue;
        o.t.push_back(t);
        o.gyro.push_back({s[i].gyro[0], s[i].gyro[1], s[i].gyro[2]});
        o.accel.push_back({s[i].accel[0], s[i].accel[1], s[i].accel[2]});
        if (o.t.size() >= 2)
            dts.push_back(o.t[o.t.size() - 1] - o.t[o.t.size() - 2]);
    }
    if (!dts.empty()) {
        std::nth_element(dts.begin(), dts.begin() + dts.size() / 2, dts.end());
        const double md = dts[dts.size() / 2];
        o.srHz = md > 1e-9 ? 1.0 / md : 0.0;
    }
    return o;
}

struct AxisMap {
    int idx[3] = {0, 1, 2};
    double sgn[3] = {1, 1, 1};
};

AxisMap parseAxisMap(const std::string& s)
{
    AxisMap m;
    size_t start = 0;
    for (int k = 0; k < 3; ++k) {
        const size_t comma = s.find(',', start);
        std::string tok = s.substr(start, comma == std::string::npos ? std::string::npos
                                                                     : comma - start);
        start = comma == std::string::npos ? s.size() : comma + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.erase(tok.begin());
        double sg = 1.0;
        if (!tok.empty() && (tok[0] == '-' || tok[0] == '+')) {
            sg = tok[0] == '-' ? -1.0 : 1.0;
            tok.erase(tok.begin());
        }
        if (tok.size() == 1 && tok[0] >= 'x' && tok[0] <= 'z') {
            m.idx[k] = tok[0] - 'x';
            m.sgn[k] = sg;
        }
    }
    return m;
}

std::array<double, 3> applyAxisMap(const AxisMap& m, const std::array<double, 3>& v)
{
    return {m.sgn[0] * v[m.idx[0]], m.sgn[1] * v[m.idx[1]], m.sgn[2] * v[m.idx[2]]};
}

// The 24 signed-axis permutations that are proper rotations (det == +1) --
// the plausible body->world gyro maps for an unknown sensor mount.
std::vector<std::string> properRotationAxisMaps()
{
    const char ax[3] = {'x', 'y', 'z'};
    auto permSign = [](const int p[3]) {
        return ((p[0] + 1) % 3 == p[1] && (p[1] + 1) % 3 == p[2]) ? 1 : -1;
    };
    std::vector<std::string> out;
    for (int p0 = 0; p0 < 3; ++p0)
        for (int p1 = 0; p1 < 3; ++p1) {
            if (p1 == p0)
                continue;
            const int p[3] = {p0, p1, 3 - p0 - p1};
            const int ps = permSign(p);
            for (int sm = 0; sm < 8; ++sm) {
                const int sg[3] = {(sm & 1) ? -1 : 1, (sm & 2) ? -1 : 1, (sm & 4) ? -1 : 1};
                if (ps * sg[0] * sg[1] * sg[2] != 1)
                    continue;
                std::string t;
                for (int k = 0; k < 3; ++k) {
                    if (k)
                        t += ',';
                    if (sg[k] < 0)
                        t += '-';
                    t += ax[p[k]];
                }
                out.push_back(t);
            }
        }
    return out;
}

// Cumulative trapezoidal integral of y(t) -> same length as t (starts at 0).
std::vector<double> cumTrapz(const std::vector<double>& t, const std::vector<double>& y)
{
    std::vector<double> o(t.size(), 0.0);
    for (size_t i = 1; i < t.size(); ++i)
        o[i] = o[i - 1] + 0.5 * (y[i] + y[i - 1]) * (t[i] - t[i - 1]);
    return o;
}

double matRotationAngleDeg(const float m[3][3])
{
    const double tr = m[0][0] + m[1][1] + m[2][2];
    return std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0)) * kRadToDeg;
}

void printSpectrumLine(const char* label, const diag::Spectrum& sp, double loHz, double hiHz)
{
    if (sp.amp.empty()) {
        std::printf("  %-10s (no spectrum -- too few samples)\n", label);
        return;
    }
    const double bandHi = std::min(hiHz, sp.sampleRateHz * 0.5);
    const diag::Peak pk = diag::dominantPeak(sp, loHz, bandHi); // peak *within* the band of interest
    const double floor = diag::noiseFloor(sp);
    const double knee = diag::bandwidthKnee(sp, floor);
    std::printf("  %-10s in-band peak %6.2f Hz @ %.4g | band[%.1f-%.1f] RMS %.4g | "
                "floor %.3g | bw-knee %5.0f Hz\n",
                label, pk.freqHz, pk.amp, loHz, bandHi, diag::bandRms(sp, loHz, bandHi), floor, knee);
}

// --- Block 0: exposure / motion-blur budget ---------------------------------
// The trailer's 0x400 record is the per-frame shutter time. Combined with the
// measured gyro rate it says how much the camera rotates *during* each frame's
// exposure -- motion blur that no geometric (per-frame or per-row) de-rotation
// can remove. Long-shutter (low-light) footage of a high-frequency mount
// resonance is blur-limited, not correction-limited.
void diagExposureBlur(const std::vector<insv::ExposureSample>& exp, const SlicedImu& s,
                      double trimIn, double trimOut, int outH, double fovDeg, const Args& a)
{
    std::printf("\n=== [0] exposure / motion-blur budget ===\n");
    std::vector<double> et;
    for (const auto& e : exp) {
        const double t = (static_cast<double>(e.timestamp) -
                          (exp.empty() ? 0.0 : static_cast<double>(exp.front().timestamp))) *
                         kImuTScale;
        if (t < trimIn || t > trimOut || e.exposure_time <= 0.0)
            continue;
        et.push_back(e.exposure_time);
    }
    if (et.empty()) {
        std::printf("  (no 0x400 exposure record in range)\n");
        return;
    }
    std::sort(et.begin(), et.end());
    const double med = et[et.size() / 2];
    const double mn = et.front(), mx = et.back();
    // Total (broadband) gyro-rate magnitude RMS over the range, deg/s.
    double sq = 0.0;
    int n = 0;
    for (size_t i = 0; i < s.t.size(); ++i) {
        const double m = std::sqrt(s.gyro[i][0] * s.gyro[i][0] + s.gyro[i][1] * s.gyro[i][1] +
                                   s.gyro[i][2] * s.gyro[i][2]);
        sq += m * m;
        ++n;
    }
    const double rateRms = n ? std::sqrt(sq / n) * kRadToDeg : 0.0;
    const double blurDeg = rateRms * med;
    const double blurPx = outH > 0 && fovDeg > 0 ? blurDeg / fovDeg * outH : 0.0;
    std::printf("  shutter (0x400)   median %.1f ms  [%.1f, %.1f]  (1/%.0f s)\n", med * 1e3, mn * 1e3,
                mx * 1e3, med > 0 ? 1.0 / med : 0.0);
    std::printf("  gyro rate |w|     %.1f deg/s RMS (broadband)\n", rateRms);
    std::printf("  => intra-frame blur ~%.2f deg  (~%.1f px at %dp / %.0f deg FOV)\n", blurDeg,
                blurPx, outH, fovDeg);
    std::printf("  (blur >~2-3 px is a visible smear that de-rotation CANNOT remove -- the only\n"
                "   levers are a faster shutter / more light at capture, or deblurring.)\n");
    (void)a;
}

// --- Block 1: raw IMU gyro/accel spectra --------------------------------------
void diagImuSpectra(const SlicedImu& s, const Args& a)
{
    std::printf("\n=== [1] raw IMU spectra  (%zu samples over %.2f s, ~%.0f Hz) ===\n", s.t.size(),
                s.t.empty() ? 0.0 : s.t.back() - s.t.front(), s.srHz);
    if (s.t.size() < 16 || s.srHz <= 0.0) {
        std::printf("  (insufficient IMU samples in range)\n");
        return;
    }
    const char* gl[3] = {"gyro.x", "gyro.y", "gyro.z"};
    const char* al[3] = {"accel.x", "accel.y", "accel.z"};
    double gyroBandSq = 0.0, gyroKneeMax = 0.0;
    for (int k = 0; k < 3; ++k) {
        std::vector<double> col(s.t.size());
        for (size_t i = 0; i < s.t.size(); ++i)
            col[i] = s.gyro[i][k]; // rad/s
        const diag::Spectrum sp = diag::amplitudeSpectrum(col, s.srHz);
        printSpectrumLine(gl[k], sp, a.stickLoHz, a.stickHiHz);
        const double b = diag::bandRms(sp, a.stickLoHz, std::min(a.stickHiHz, s.srHz * 0.5));
        gyroBandSq += b * b;
        gyroKneeMax = std::max(gyroKneeMax, diag::bandwidthKnee(sp, diag::noiseFloor(sp)));
    }
    for (int k = 0; k < 3; ++k) {
        std::vector<double> col(s.t.size());
        for (size_t i = 0; i < s.t.size(); ++i)
            col[i] = s.accel[i][k]; // g
        printSpectrumLine(al[k], diag::amplitudeSpectrum(col, s.srHz), a.stickLoHz, a.stickHiHz);
    }
    const double gyroBand = std::sqrt(gyroBandSq);
    std::printf("  --> gyro in-band [%.1f-%.1f Hz] rate %.4g rad/s RMS (%.3g deg/s); "
                "widest bw-knee %.0f Hz\n",
                a.stickLoHz, a.stickHiHz, gyroBand, gyroBand * kRadToDeg, gyroKneeMax);
    std::printf("  (a real stick swing => a gyro in-band peak clearly above 'floor' and a\n"
                "   bw-knee well past %.0f Hz. A knee near ~2-3 Hz with a flat tail means the\n"
                "   camera has low-pass-filtered the swing out of its own IMU.)\n",
                a.stickHiHz);
}

// --- Block 6: Horizon-lock track vs. gravity --------------------------------
// Builds a HorizonLock track and, on "quiet" samples (low-passed accel within
// 1 +- 0.12 g, not turning -- the gating build()'s accel anchor uses), compares
// the track's world up with the measured gravity, both in a level heading
// frame. A tilt here is the track's own error (seed / anchor); ~0 means the
// track is level and any visible tilt comes from the IMU->sphere mapping.
void diagHorizonVsGravity(const insv::ImuTrack& imu, double t0Sec, double t1Sec, const Args& a)
{
    insv::StabConfig hc = insv::StabConfig::preset(insv::StabMode::HorizonLock);
    hc.gyroAxes = a.gyroAxes.empty() ? imu.axisMap : a.gyroAxes;
    insv::GyroStabilizer hl;
    if (a.noAutoSync)
        hl.build(imu.samples, hc);
    else
        hl.build(imu, hc, a.syncAdjustMs / 1000.0);
    std::printf("\n=== [6] horizon-lock track vs. gravity  (map '%s') ===\n", hc.gyroAxes.c_str());
    if (!hl.ready()) {
        std::printf("  (no track)\n");
        return;
    }
    std::vector<insv::ImuSample> s = imu.samples;
    std::sort(s.begin(), s.end(), [](const insv::ImuSample& x, const insv::ImuSample& y) {
        return x.timestamp < y.timestamp;
    });
    const AxisMap am = parseAxisMap(hc.gyroAxes);
    const double ts0 = static_cast<double>(s.front().timestamp);
    const double off = hl.effectiveTimeOffsetSec();
    const double rate = hl.clockRatePpm() * 1e-6;
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    double aLP[3] = {0, 0, 0};
    bool init = false;
    double yawLp = 0.0;
    constexpr double kBin = 10.0;
    struct Acc {
        double r = 0, p = 0;
        long n = 0, all = 0;
    };
    Acc total;
    std::vector<Acc> bins;
    for (size_t i = 1; i < s.size(); ++i) {
        const double tg = (static_cast<double>(s[i].timestamp) - ts0) * kImuTScale;
        const double dt = (static_cast<double>(s[i].timestamp) -
                           static_cast<double>(s[i - 1].timestamp)) * kImuTScale;
        const std::array<double, 3> ac =
            applyAxisMap(am, {s[i].accel[0], s[i].accel[1], s[i].accel[2]});
        if (!init) {
            for (int k = 0; k < 3; ++k)
                aLP[k] = ac[k];
            init = true;
        } else if (dt > 0.0) {
            const double al = 1.0 - std::exp(-kTwoPi * hc.accelLowPassHz * dt);
            for (int k = 0; k < 3; ++k)
                aLP[k] += al * (ac[k] - aLP[k]);
        }
        const double tv = (tg - off) / (1.0 + rate);
        const auto q = hl.rawOrientation(tv);
        const double w = q[0], x = q[1], y = q[2], z = q[3];
        const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                                {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                                {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
        const std::array<double, 3> g =
            applyAxisMap(am, {s[i].gyro[0], s[i].gyro[1], s[i].gyro[2]});
        const double worldYaw = R[1][0] * g[0] + R[1][1] * g[1] + R[1][2] * g[2];
        if (dt > 0.0)
            yawLp += (1.0 - std::exp(-kTwoPi * 0.5 * dt)) * (worldYaw - yawLp);
        if (tv < t0Sec || tv > t1Sec)
            continue;
        const size_t b = static_cast<size_t>((tv - t0Sec) / kBin);
        if (bins.size() <= b)
            bins.resize(b + 1);
        ++bins[b].all;
        ++total.all;
        const double m = std::sqrt(aLP[0] * aLP[0] + aLP[1] * aLP[1] + aLP[2] * aLP[2]);
        if (std::fabs(m - 1.0) > 0.12 || std::fabs(yawLp) > 0.08)
            continue;
        double u[3]; // measured up, world frame
        for (int r = 0; r < 3; ++r)
            u[r] = (R[r][0] * aLP[0] + R[r][1] * aLP[1] + R[r][2] * aLP[2]) / m;
        // Level heading frame from the body forward (-Z).
        double fx = -R[0][2], fz = -R[2][2];
        const double fn = std::sqrt(fx * fx + fz * fz);
        if (fn < 1e-3)
            continue;
        fx /= fn;
        fz /= fn;
        const double rollDeg = std::asin(std::clamp(-fz * u[0] + fx * u[2], -1.0, 1.0)) * kRadToDeg;
        const double pitchDeg = std::asin(std::clamp(fx * u[0] + fz * u[2], -1.0, 1.0)) * kRadToDeg;
        for (Acc* acc : {&total, &bins[b]}) {
            acc->r += rollDeg;
            acc->p += pitchDeg;
            ++acc->n;
        }
    }
    std::printf("  tilt of measured gravity vs. the track's up, in the forward view\n"
                "  (roll: + = gravity leans right; pitch: + = gravity leans forward)\n");
    std::printf("  %8s %8s %9s %9s\n", "t (s)", "quiet%", "roll(deg)", "pitch(deg)");
    for (size_t b = 0; b < bins.size(); ++b) {
        const Acc& acc = bins[b];
        if (acc.n == 0)
            std::printf("  %8.0f %7.0f%% %9s %9s\n", t0Sec + b * kBin, 0.0, "-", "-");
        else
            std::printf("  %8.0f %7.0f%% %9.2f %9.2f\n", t0Sec + b * kBin,
                        100.0 * acc.n / std::max(1L, acc.all), acc.r / acc.n, acc.p / acc.n);
    }
    if (total.n > 0)
        std::printf("  %8s %7.0f%% %9.2f %9.2f\n", "all", 100.0 * total.n / std::max(1L, total.all),
                    total.r / total.n, total.p / total.n);
}

// --- Block 2: what GyroStabilizer removes vs. the raw rotation ---------------
void diagIntegratedVsApplied(const SlicedImu& s, const insv::GyroStabilizer& gyro,
                             const insv::StabConfig& gcfg, const Args& a)
{
    std::printf("\n=== [2] raw rotation vs. applied correction  (map '%s', scale %.2f) ===\n",
                gcfg.gyroAxes.c_str(), gcfg.gyroScale);
    if (s.t.size() < 16 || s.srHz <= 0.0) {
        std::printf("  (insufficient IMU samples in range)\n");
        return;
    }
    const AxisMap am = parseAxisMap(gcfg.gyroAxes);
    // Raw: axis-map the gyro rate to world axes, integrate each to an angle.
    std::array<std::vector<double>, 3> rawRate, rawAngle;
    for (int k = 0; k < 3; ++k)
        rawRate[k].resize(s.t.size());
    for (size_t i = 0; i < s.t.size(); ++i) {
        const auto w = applyAxisMap(am, s.gyro[i]);
        for (int k = 0; k < 3; ++k)
            rawRate[k][i] = w[k] * gcfg.gyroScale;
    }
    for (int k = 0; k < 3; ++k)
        rawAngle[k] = cumTrapz(s.t, rawRate[k]);

    // Applied: sample the stabilizer's de-rotation at the IMU grid, decompose.
    std::array<std::vector<double>, 3> appYpr; // yaw,pitch,roll (deg)
    std::vector<double> appAngle(s.t.size());
    for (int k = 0; k < 3; ++k)
        appYpr[k].resize(s.t.size());
    for (size_t i = 0; i < s.t.size(); ++i) {
        float m[3][3];
        gyro.correctionMatrix(s.t[i], m);
        float y, p, r;
        insv::MatrixToYawPitchRoll(m, y, p, r);
        appYpr[0][i] = y * kRadToDeg;
        appYpr[1][i] = p * kRadToDeg;
        appYpr[2][i] = r * kRadToDeg;
        appAngle[i] = matRotationAngleDeg(m);
    }

    const char* rl[3] = {"raw.wx", "raw.wy", "raw.wz"};
    const char* pl[3] = {"corr.yaw", "corr.pitch", "corr.roll"};
    for (int k = 0; k < 3; ++k) {
        std::vector<double> deg(s.t.size());
        for (size_t i = 0; i < s.t.size(); ++i)
            deg[i] = rawAngle[k][i] * kRadToDeg;
        printSpectrumLine(rl[k], diag::amplitudeSpectrum(deg, s.srHz), a.stickLoHz, a.stickHiHz);
    }
    for (int k = 0; k < 3; ++k)
        printSpectrumLine(pl[k], diag::amplitudeSpectrum(appYpr[k], s.srHz), a.stickLoHz,
                          a.stickHiHz);

    // The headline numbers: stick-band energy the raw rotation carries vs. what
    // the correction actually applies. corr >= raw in-band => stabilizer is
    // removing the swing. corr << raw in-band => it is NOT (window too short,
    // wrong axis map/scale, or a sync error) -- follow up in block [3].
    double rawBand = 0.0;
    for (int k = 0; k < 3; ++k) {
        std::vector<double> deg(s.t.size());
        for (size_t i = 0; i < s.t.size(); ++i)
            deg[i] = rawAngle[k][i] * kRadToDeg;
        const double b = diag::bandRms(diag::amplitudeSpectrum(deg, s.srHz), a.stickLoHz, a.stickHiHz);
        rawBand += b * b;
    }
    rawBand = std::sqrt(rawBand);
    double appBand = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double b = diag::bandRms(diag::amplitudeSpectrum(appYpr[k], s.srHz), a.stickLoHz,
                                       a.stickHiHz);
        appBand += b * b;
    }
    appBand = std::sqrt(appBand);
    std::printf("  --> stick-band [%.1f-%.1f Hz]: raw rotation %.4g deg RMS, "
                "applied correction %.4g deg RMS  (ratio %.2f)\n",
                a.stickLoHz, a.stickHiHz, rawBand, appBand, rawBand > 1e-9 ? appBand / rawBand : 0.0);

    if (!a.diagCsv.empty()) {
        FILE* f = std::fopen(a.diagCsv.c_str(), "w");
        if (!f) {
            std::printf("  (could not open --diag-csv '%s')\n", a.diagCsv.c_str());
        } else {
            std::fprintf(f, "t_s,raw_wx_deg,raw_wy_deg,raw_wz_deg,corr_yaw_deg,corr_pitch_deg,"
                            "corr_roll_deg,corr_angle_deg\n");
            for (size_t i = 0; i < s.t.size(); ++i)
                std::fprintf(f, "%.6f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n", s.t[i],
                             rawAngle[0][i] * kRadToDeg, rawAngle[1][i] * kRadToDeg,
                             rawAngle[2][i] * kRadToDeg, appYpr[0][i], appYpr[1][i], appYpr[2][i],
                             appAngle[i]);
            std::fclose(f);
            std::printf("  wrote per-sample tracks -> %s\n", a.diagCsv.c_str());
        }
    }
}

// --- Block 3: image-derived forward-view pan vs. the gyro -------------------
// Uses the estimator's already-collected forward-view pan path (accumulatedYaw/
// Pitch, one entry per analysed frame interval) and the full IMU track (to
// rebuild candidate gyro configs cheaply -- no re-stitch).
void diagImageVsGyro(const insv::ResidualPanEstimator& est,
                     const std::vector<insv::ImuSample>& imu, const insv::StabConfig& baseCfg,
                     double fps, const Args& a)
{
    std::printf("\n=== [3] image-derived pan vs. gyro ===\n");
    const std::vector<double>& it = est.times();
    if (it.size() < 16) {
        std::printf("  (too few analysed intervals: %zu)\n", it.size());
        return;
    }
    std::vector<double> dts;
    for (size_t i = 1; i < it.size(); ++i)
        dts.push_back(it[i] - it[i - 1]);
    std::nth_element(dts.begin(), dts.begin() + dts.size() / 2, dts.end());
    const double imgSr = dts[dts.size() / 2] > 1e-9 ? 1.0 / dts[dts.size() / 2] : fps;

    // Image pan path (position, radians -> deg) and its spectrum.
    auto toDeg = [](const std::vector<double>& v) {
        std::vector<double> o(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            o[i] = v[i] * kRadToDeg;
        return o;
    };
    const std::vector<double> imgYaw = toDeg(est.accumulatedYaw());
    const std::vector<double> imgPitch = toDeg(est.accumulatedPitch());
    const diag::Spectrum imgYawSp = diag::amplitudeSpectrum(imgYaw, imgSr);
    const diag::Spectrum imgPitchSp = diag::amplitudeSpectrum(imgPitch, imgSr);
    std::printf("  image sample rate ~%.1f Hz (Nyquist %.1f Hz)\n", imgSr, imgSr * 0.5);
    printSpectrumLine("img.yaw", imgYawSp, a.stickLoHz, std::min(a.stickHiHz, imgSr * 0.5));
    printSpectrumLine("img.pitch", imgPitchSp, a.stickLoHz, std::min(a.stickHiHz, imgSr * 0.5));

    const double imgBand =
        std::sqrt(std::pow(diag::bandRms(imgYawSp, a.stickLoHz, std::min(a.stickHiHz, imgSr * 0.5)), 2) +
                  std::pow(diag::bandRms(imgPitchSp, a.stickLoHz, std::min(a.stickHiHz, imgSr * 0.5)), 2));

    // Candidate sweep: axis map x scale x time-offset, scored against the image
    // pan-rate. Cheap: build() re-integrates ~1 kHz for the clip (few ms),
    // correctionMatrix() sampled only at the interval times.
    auto imgRate = [](const std::vector<double>& pos) {
        std::vector<double> r(pos.size(), 0.0);
        for (size_t i = 1; i < pos.size(); ++i)
            r[i] = pos[i] - pos[i - 1];
        return r;
    };
    const std::vector<double> imgYawRate = imgRate(est.accumulatedYaw());
    const std::vector<double> imgPitchRate = imgRate(est.accumulatedPitch());

    const std::vector<std::string> maps = properRotationAxisMaps();
    const double scales[5] = {0.5, 0.75, 1.0, 1.5, 2.0};

    struct Cand {
        std::string map;
        double scale = 1.0;
        double lagSec = 0.0; // image lag vs. applied gyro at best correlation (sign not yet confirmed)
        double score = -2.0; // |corr(app yaw-rate, img yaw-rate)| + pitch, best over the shift
        double bandRatio = 0.0;
    };
    std::vector<Cand> ranked;
    insv::GyroStabilizer g;
    for (const std::string& mp : maps) {
        for (double sc : scales) {
            insv::StabConfig c = baseCfg;
            c.gyroAxes = mp;
            c.gyroScale = static_cast<float>(sc);
            g.build(imu, c); // keeps baseCfg's (effective) video<->IMU offset
            if (!g.ready())
                continue;
            // applied yaw/pitch (deg) at each interval time, then rate.
            std::vector<double> ay(it.size()), ap(it.size());
            for (size_t i = 0; i < it.size(); ++i) {
                float m[3][3];
                g.correctionMatrix(it[i], m);
                float y, p, r;
                insv::MatrixToYawPitchRoll(m, y, p, r);
                ay[i] = y * kRadToDeg;
                ap[i] = p * kRadToDeg;
            }
            std::vector<double> ayr(it.size(), 0.0), apr(it.size(), 0.0);
            for (size_t i = 1; i < it.size(); ++i) {
                ayr[i] = ay[i] - ay[i - 1];
                apr[i] = ap[i] - ap[i - 1];
            }
            // Best correlation over a +-100 ms integer-sample shift.
            Cand best;
            best.map = mp;
            best.scale = sc;
            const int maxShift =
                std::max(1, std::min<int>(8, static_cast<int>(std::lround(0.1 * imgSr))));
            for (int sh = -maxShift; sh <= maxShift; ++sh) {
                auto shifted = [&](const std::vector<double>& v) {
                    std::vector<double> o(v.size(), 0.0);
                    for (int i = 0; i < static_cast<int>(v.size()); ++i) {
                        const int j = i + sh;
                        if (j >= 0 && j < static_cast<int>(v.size()))
                            o[i] = v[j];
                    }
                    return o;
                };
                const double c1 = std::fabs(diag::pearson(shifted(ayr), imgYawRate));
                const double c2 = std::fabs(diag::pearson(shifted(apr), imgPitchRate));
                const double sc2 = c1 + c2;
                if (sc2 > best.score) {
                    best.score = sc2;
                    best.lagSec = sh / imgSr; // applied[i+sh] ~ image[i]
                }
            }
            // in-band applied energy (yaw+pitch) for context
            const double ab =
                std::sqrt(std::pow(diag::bandRms(diag::amplitudeSpectrum(ay, imgSr), a.stickLoHz,
                                                 std::min(a.stickHiHz, imgSr * 0.5)),
                                   2) +
                          std::pow(diag::bandRms(diag::amplitudeSpectrum(ap, imgSr), a.stickLoHz,
                                                 std::min(a.stickHiHz, imgSr * 0.5)),
                                   2));
            best.bandRatio = imgBand > 1e-9 ? ab / imgBand : 0.0;
            ranked.push_back(best);
        }
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Cand& x, const Cand& y) { return x.score > y.score; });

    std::printf("  image in-band pan  %.4g deg RMS  (yaw+pitch, [%.1f-%.1f Hz])\n", imgBand,
                a.stickLoHz, std::min(a.stickHiHz, imgSr * 0.5));
    std::printf("  top gyro configs by |corr(applied rate, image rate)| (max 2.0):\n");
    std::printf("    %-14s %-6s %-9s %-7s %s\n", "axis-map", "scale", "img-lag(s)", "corr",
                "app/img in-band");
    for (size_t i = 0; i < ranked.size() && i < 6; ++i)
        std::printf("    %-14s %-6.2f %+9.3f %-7.3f %.2f\n", ranked[i].map.c_str(), ranked[i].scale,
                    ranked[i].lagSec, ranked[i].score, ranked[i].bandRatio);

    // Tentative verdict from this clip (what to work on next; confirm across clips).
    const bool haveCorr = !ranked.empty();
    const double bestCorr = haveCorr ? ranked.front().score : 0.0;      // 0..2
    const double bestRatio = haveCorr ? ranked.front().bandRatio : 0.0; // applied/image in-band
    std::printf("  VERDICT: ");
    if (bestCorr >= 0.8 && bestRatio >= 0.6 && bestRatio <= 1.8) {
        std::printf("the swing IS in the IMU and the gyro path already captures it well\n"
                    "  (corr %.2f/2.0, applied/image in-band %.2f). Next: calibration/\n"
                    "  sync/window tuning, not image work.\n",
                    bestCorr, bestRatio);
    } else if (bestCorr >= 0.8) {
        std::printf("the swing is in the IMU but under/over-applied (corr %.2f/2.0, "
                    "applied/image in-band %.2f).\n  Next: fix the axis-map/scale/window "
                    "shown above.\n",
                    bestCorr, bestRatio);
    } else if (bestCorr > 0.0) {
        std::printf("no gyro config tracks the image well (best corr %.2f/2.0). If block [1]'s\n"
                    "  gyro bw-knee is low and block [2]'s ratio << 1, the camera filtered the\n"
                    "  swing out => next: image+gyro fusion via "
                    "GyroStabilizer::buildFromOrientation.\n",
                    bestCorr);
    } else {
        std::printf("inconclusive (no candidates scored).\n");
    }
}

// --- Block 5: video<->IMU time sync --------------------------------------
// Primary: the trailer's 0x400 exposure record carries one device-clock
// timestamp per video frame, on the SAME clock as the IMU (0x300). So
//   timeOffsetSec = median_n [ exposure[n].ts - imu[0].ts - n / fps ]
// is a direct read of how far the GyroStabilizer track origin (imu[0]) sits
// from video frame 0 -- no image analysis needed. Cross-check: a lock-in
// between the raw forward-view image motion (`estRaw`, fed IDENTITY
// de-rotation) and the axis-mapped gyro, demodulated at the stick resonance.
void diagVideoImuSync(const insv::ResidualPanEstimator& estRaw,
                      const std::vector<insv::ImuSample>& imu,
                      const std::vector<insv::ExposureSample>& exposure, double fps,
                      const insv::StabConfig& cfg, const Args& a)
{
    std::printf("\n=== [5] video<->IMU time sync  (map '%s', scale %.2f) ===\n",
                cfg.gyroAxes.c_str(), cfg.gyroScale);

    // --- direct: exposure-record timestamps vs frame index ---------------
    if (exposure.size() >= 32 && fps > 1.0) {
        const double imu0 = static_cast<double>(imu.front().timestamp) * kImuTScale; // s
        const double frameDt = 1.0 / fps;
        // Least-squares line  off(n) = c + b*n  over off(n) = exp[n].ts - imu0 - n*frameDt.
        const size_t N = exposure.size();
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        std::vector<double> off(N);
        for (size_t n = 0; n < N; ++n) {
            off[n] = static_cast<double>(exposure[n].timestamp) * kImuTScale - imu0 -
                     static_cast<double>(n) * frameDt;
            const double xn = static_cast<double>(n);
            sx += xn;
            sy += off[n];
            sxx += xn * xn;
            sxy += xn * off[n];
        }
        const double dn = static_cast<double>(N);
        const double den = dn * sxx - sx * sx;
        const double slope = den != 0.0 ? (dn * sxy - sx * sy) / den : 0.0; // s per frame index
        const double icept = (sy - slope * sx) / dn;
        std::vector<double> so = off;
        std::nth_element(so.begin(), so.begin() + so.size() / 2, so.end());
        const double med = so[so.size() / 2];
        std::printf("  exposure record: %zu frames, dt %.3f ms (video frame %.3f ms) => %s\n",
                    N, (frameDt + slope) * 1e3, frameDt * 1e3,
                    std::fabs(slope) * 1e3 < 0.05 ? "frame-locked" : "NOT frame-locked (drift)");
        std::printf("  --> timeOffsetSec = %+.4f  (%.1f ms; median %+.1f ms, spread %.1f ms)\n",
                    icept, icept * 1e3, med * 1e3, (so.back() - so.front()) * 1e3);
        std::printf("      Positive = sample the gyro track that many seconds AFTER the video\n"
                    "      frame time. (assumes exposure[0] <-> frame 0; a lead/lag of k frames\n"
                    "      would shift this by k*%.1f ms.)\n",
                    frameDt * 1e3);
    } else {
        std::printf("  (no usable 0x400 exposure record -- direct sync unavailable)\n");
    }

    std::printf("  -- cross-check: image/gyro lock-in --\n");
    const std::vector<double>& t = estRaw.times();
    const std::vector<double>& iy = estRaw.accumulatedYaw();
    const std::vector<double>& ip = estRaw.accumulatedPitch();
    if (t.size() < 32 || iy.size() != t.size()) {
        std::printf("  (too few raw-image intervals: %zu)\n", t.size());
        return;
    }
    const size_t M = t.size() - 1;
    std::vector<double> tm(M), dIy(M), dIp(M);
    for (size_t i = 0; i < M; ++i) {
        tm[i] = 0.5 * (t[i] + t[i + 1]);
        dIy[i] = iy[i + 1] - iy[i];
        dIp[i] = ip[i + 1] - ip[i];
    }
    const double imgSr = (t.back() - t.front()) > 1e-6 ? M / (t.back() - t.front()) : 30.0;

    // World gyro rate (axis-mapped, scaled) on its native ~1 kHz grid.
    const AxisMap am = parseAxisMap(cfg.gyroAxes);
    const double sc = cfg.gyroScale != 0.0f ? static_cast<double>(cfg.gyroScale) : 1.0;
    const double ts0 = static_cast<double>(imu.front().timestamp);
    const double off0 = cfg.timeOffsetSec; // effective video->IMU offset; sweep around it
    std::vector<double> gt;
    std::vector<std::array<double, 3>> gw;
    gt.reserve(imu.size());
    gw.reserve(imu.size());
    for (const auto& s : imu) {
        const double tt = (static_cast<double>(s.timestamp) - ts0) * kImuTScale;
        if (tt < t.front() + off0 - 0.15 || tt > t.back() + off0 + 0.15)
            continue;
        const auto w = applyAxisMap(am, {s.gyro[0], s.gyro[1], s.gyro[2]});
        gt.push_back(tt);
        gw.push_back({w[0] * sc, w[1] * sc, w[2] * sc});
    }
    if (gt.size() < 64) {
        std::printf("  (too few IMU samples in range: %zu)\n", gt.size());
        return;
    }

    // Integral of world-rate component k over [ta, tb] (trapezoid on the grid).
    auto integ = [&](double ta, double tb, int k) {
        if (tb <= ta)
            return 0.0;
        double acc = 0.0;
        for (size_t j = 1; j < gt.size(); ++j) {
            const double a0 = gt[j - 1], a1 = gt[j];
            if (a1 <= ta || a0 >= tb || a1 <= a0)
                continue;
            const double lo = std::max(a0, ta), hi = std::min(a1, tb);
            auto at = [&](double x) {
                const double f = (x - a0) / (a1 - a0);
                return gw[j - 1][k] + f * (gw[j][k] - gw[j - 1][k]);
            };
            acc += 0.5 * (at(lo) + at(hi)) * (hi - lo);
        }
        return acc;
    };
    // World Y -> yaw, world X -> pitch (small-angle; sign folds into the phase).
    auto gyroInc = [&](double d, std::vector<double>& gy, std::vector<double>& gp) {
        gy.assign(M, 0.0);
        gp.assign(M, 0.0);
        for (size_t i = 0; i < M; ++i) {
            gy[i] = integ(t[i] + off0 + d, t[i + 1] + off0 + d, 1);
            gp[i] = integ(t[i] + off0 + d, t[i + 1] + off0 + d, 0);
        }
    };

    std::vector<double> gy0, gp0;
    gyroInc(0.0, gy0, gp0);
    const double hiHz = std::min(a.stickHiHz, imgSr * 0.5 - 0.5);
    const diag::Peak pk = diag::dominantPeak(diag::amplitudeSpectrum(gy0, imgSr), a.stickLoHz, hiHz);
    const double fpk = pk.freqHz > 0.5 ? pk.freqHz : 0.5 * (a.stickLoHz + hiHz);
    std::printf("  image sampled ~%.1f Hz; sync resonance f = %.2f Hz\n", imgSr, fpk);

    // Lock-in: x(t - tau) has FT phase -2*pi*f*tau, so
    // tau = -arg(Z_img * conj(Z_gyro)) / (2*pi*f).
    auto demod = [&](const std::vector<double>& v) {
        std::complex<double> z{0.0, 0.0};
        for (size_t i = 0; i < M; ++i)
            z += v[i] * std::exp(std::complex<double>(0.0, -2.0 * diag::kPi * fpk * tm[i]));
        return z;
    };
    auto tauFrom = [&](std::complex<double> zi, std::complex<double> zg) {
        double tau = -std::arg(zi * std::conj(zg)) / (2.0 * diag::kPi * fpk);
        const double period = 1.0 / fpk;
        while (tau > 0.5 * period)
            tau -= period;
        while (tau < -0.5 * period)
            tau += period;
        return tau;
    };
    const std::complex<double> Ziy = demod(dIy), Zip = demod(dIp);
    const std::complex<double> Zgy = demod(gy0), Zgp = demod(gp0);
    const double tauY = tauFrom(Ziy, Zgy), tauP = tauFrom(Zip, Zgp);
    const double wY = std::abs(Ziy) * std::abs(Zgy), wP = std::abs(Zip) * std::abs(Zgp);
    const double tauLock = (wY + wP) > 1e-20 ? (tauY * wY + tauP * wP) / (wY + wP) : 0.0;
    std::printf("  lock-in delay:   yaw %+6.1f ms   pitch %+6.1f ms   (weighted %+6.1f ms)\n",
                tauY * 1e3, tauP * 1e3, tauLock * 1e3);

    // Coarse sweep with a band-emphasising high-pass (kills the slow drift the
    // raw increments carry, leaves the resonance the sync rides on).
    auto hp = [](const std::vector<double>& x) {
        constexpr int w = 3;
        std::vector<double> o(x.size(), 0.0);
        for (size_t i = 0; i < x.size(); ++i) {
            double s = 0.0;
            int n = 0;
            for (int k = -w; k <= w; ++k) {
                const long j = static_cast<long>(i) + k;
                if (j >= 0 && j < static_cast<long>(x.size())) {
                    s += x[static_cast<size_t>(j)];
                    ++n;
                }
            }
            o[i] = x[i] - s / n;
        }
        return o;
    };
    const std::vector<double> hIy = hp(dIy), hIp = hp(dIp);
    std::vector<double> gy, gp;
    auto scoreAt = [&](double d) {
        gyroInc(d, gy, gp);
        return std::fabs(diag::pearson(hIy, hp(gy))) + std::fabs(diag::pearson(hIp, hp(gp)));
    };
    double best = -1e9, bestD = 0.0, at0 = 0.0;
    for (int ms = -60; ms <= 60; ++ms) {
        const double s = scoreAt(ms * 1e-3);
        if (ms == 0)
            at0 = s;
        if (s > best) {
            best = s;
            bestD = ms * 1e-3;
        }
    }
    { // parabolic refine
        const double h = 2e-3;
        const double sm = scoreAt(bestD - h), s0 = scoreAt(bestD), sp = scoreAt(bestD + h);
        const double den = sm - 2 * s0 + sp;
        if (std::fabs(den) > 1e-12)
            bestD += 0.5 * h * (sm - sp) / den;
    }
    std::printf("  (delays relative to the effective offset %+.1f ms)\n", off0 * 1e3);
    std::printf("  sweep best delay %+6.1f ms   band-corr %.3f/2.0  (vs %.3f at 0 ms)\n",
                bestD * 1e3, best, at0);
    if (best < 0.35)
        std::printf("  (lock-in confidence LOW -- the forward-view image estimator rolls off\n"
                    "   below the resonance; trust the exposure-record number above.)\n");
}

} // namespace

int main(int argc, char** argv)
{
    Args a = parseArgs(argc, argv);
    if (!a.stabIn.empty()) { // fail before any decoder/GPU thread exists
        FILE* f = std::fopen(a.stabIn.c_str(), "r");
        if (!f)
            die("could not open --stab-in");
        std::fclose(f);
    }

    std::string err;
    stitch::ClipProbe probe;
    if (!stitch::ProbeClip(a.input, probe, err))
        die(err);
    const double fps = probe.fps > 0.0 ? probe.fps : 30.0;
    const int64_t trimIn = std::max<int64_t>(0, a.trimInMs);
    const int64_t trimOut = a.trimOutMs < 0 ? probe.duration_ms
                                            : std::min<int64_t>(a.trimOutMs, probe.duration_ms);
    if (trimOut - trimIn < 200)
        die("analysed range shorter than 200 ms");

    // --- per-clip sync fine-tune ---------------------------------------
    if (a.autoSync) {
        const stitch::ClipSyncResult sr = stitch::MeasureClipSync(a.input, trimIn, trimOut);
        std::printf("auto-sync: %s\n", sr.message.c_str());
        if (sr.ok)
            a.syncAdjustMs = sr.adjustMs;
    }

    // --- gyro track --------------------------------------------------------
    insv::StabConfig gcfg = gyroConfig(a);
    insv::GyroStabilizer gyro;
    // The camera's track, lined up with the video as the app does; the
    // exposure record only for the diagnostics that read it (Insta360 files).
    std::optional<insv::ImuTrack> imu;
    std::vector<insv::ExposureSample> exposure;
    // The configs the diagnostics print and re-map with: the track's own axis
    // map unless --gyro-axes overrides it.
    auto withAxes = [&imu](insv::StabConfig c) {
        if (c.gyroAxes.empty() && imu)
            c.gyroAxes = imu->axisMap;
        return c;
    };
    if (gcfg.mode != insv::StabMode::Off || a.diagnose || a.rsStitchMs[0] != 0.0f ||
        a.rsStitchMs[1] != 0.0f) {
        imu = camera::ReadClipImu(a.input);
        if (imu && a.diagnose)
            if (auto tele = insv::ReadInsvTelemetry(a.input))
                exposure = std::move(tele->exposure);
        if (imu) {
            gcfg = withAxes(gcfg);
            const insv::StabConfig bc =
                gcfg.mode != insv::StabMode::Off
                    ? gcfg
                    : withAxes(insv::StabConfig::preset(insv::StabMode::LeanFollow));
            if (a.noAutoSync)
                gyro.build(imu->samples, bc);
            else
                gyro.build(*imu, bc, a.syncAdjustMs / 1000.0);
            std::printf("video<->IMU offset: %+.1f ms, frame clock %+.1f ppm %s\n",
                        gyro.effectiveTimeOffsetSec() * 1e3, gyro.clockRatePpm(),
                        a.noAutoSync ? "(--no-auto-sync)" : "(the file's timing)");
        } else {
            std::printf("note: no usable IMU track -- residual runs against the raw image\n");
        }
    }
    std::printf("gyro:   %s\n", gyro.ready() ? "built" : "off / unavailable");
    if (!a.dumpTrack.empty() && gyro.ready()) {
        FILE* f = std::fopen(a.dumpTrack.c_str(), "w");
        if (!f)
            die("could not open --dump-track");
        for (double t = trimIn / 1000.0 - 5.0; t <= trimOut / 1000.0 + 5.0; t += 0.001) {
            const auto q = gyro.rawOrientation(t);
            std::fprintf(f, "%.4f %.9f %.9f %.9f %.9f\n", t, q[0], q[1], q[2], q[3]);
        }
        std::fclose(f);
        std::printf("wrote raw orientation track -> %s\n", a.dumpTrack.c_str());
    }

    // --- diagnostic blocks [1] and [2] (IMU-only, no stitch needed) --------
    if (a.diagnose && imu) {
        const SlicedImu sImu = sliceImu(imu->samples, trimIn / 1000.0, trimOut / 1000.0);
        diagExposureBlur(exposure, sImu, trimIn / 1000.0, trimOut / 1000.0, a.outH, a.fovDeg,
                         a);
        diagImuSpectra(sImu, a);
        if (gyro.ready()) {
            const insv::StabConfig used =
                gcfg.mode != insv::StabMode::Off
                    ? gcfg
                    : withAxes(insv::StabConfig::preset(insv::StabMode::LeanFollow));
            diagIntegratedVsApplied(sImu, gyro, used, a);
        } else {
            std::printf("\n=== [2] skipped -- no gyro track ===\n");
        }
        diagHorizonVsGravity(*imu, trimIn / 1000.0, trimOut / 1000.0, a);
    }

    // --- analysis pass: stitch small, grayscale, estimate ------------------
    const int aw = a.workW, ah = a.workW / 2;
    stitch::PreviewStitchConfig asc;
    asc.width = aw;
    asc.height = ah;
    auto asrc = std::make_unique<stitch::NativeStitchSource>();
    if (!asrc->Open(a.input, asc) || !asrc->SeekTo(trimIn))
        die(asrc->GetLastError());

    insv::ResidualPanEstimator est;
    // Block [4]: a second estimator over a wider forward cone -- picks up more
    // peripheral / down-angled (nearer) scene content. Comparing its
    // post-de-rotation residual to `est`'s narrow-forward residual is a rough
    // far-field(rotation) vs near-field(parallax) split.
    insv::ResidualPanEstimator::Params wideP;
    wideP.forwardZMax = -0.15;   // ~forward 80 deg cone vs the default ~60 deg
    wideP.poleMarginFrac = 0.22; // still keep the true equirect caps out
    auto estWide = a.diagnose ? std::make_unique<insv::ResidualPanEstimator>(wideP) : nullptr;
    // Block [5]: raw image motion (fed IDENTITY de-rotation) for the time-sync
    // lock-in -- `est` above is fed the LeanFollow de-rotation, which would
    // partly cancel the very band the sync rides on.
    auto estRaw = a.diagnose ? std::make_unique<insv::ResidualPanEstimator>() : nullptr;

    std::vector<uint8_t> gray(static_cast<size_t>(aw) * ah);
    const double halfFrameMs = 500.0 / fps;
    int64_t analysed = 0;
    for (;;) {
        stitch::Frame f = asrc->ReadFrame();
        if (f.rgba.empty()) {
            if (!asrc->GetLastError().empty())
                die(asrc->GetLastError());
            break;
        }
        if (f.timestamp_ms + halfFrameMs < trimIn)
            continue;
        if (f.timestamp_ms > trimOut)
            break;

        float g[3][3];
        if (gyro.ready())
            gyro.correctionMatrix(f.timestamp_ms / 1000.0, g);
        else
            identity3(g);

        rgbaToGray(f.rgba.data(), gray.data(), aw * ah);
        est.addFrame(gray.data(), aw, ah, f.timestamp_ms / 1000.0, g);
        if (estWide)
            estWide->addFrame(gray.data(), aw, ah, f.timestamp_ms / 1000.0, g);
        if (estRaw) {
            float gi[3][3];
            identity3(gi);
            estRaw->addFrame(gray.data(), aw, ah, f.timestamp_ms / 1000.0, gi);
        }
        if ((++analysed % 60) == 0) {
            std::printf("\r  analyse %lld frames", static_cast<long long>(analysed));
            std::fflush(stdout);
        }
    }
    std::printf("\r  analyse %lld frames\n", static_cast<long long>(analysed));
    if (est.intervalCount() < 2) {
        if (a.diagnose) {
            std::printf("\n(too few analysable frames -- blocks [3]/[4] skipped; "
                        "[1]/[2] above are IMU-only and still valid)\n");
            return 0;
        }
        die("too few analysable frames");
    }

    const insv::ResidualTrack track = est.finish(a.residStrength, fps);

    // --- report ----------------------------------------------------------
    const double rawYawRms = rmsFirstDiff(est.accumulatedYaw()) * kRadToDeg;
    const double rawPitchRms = rmsFirstDiff(est.accumulatedPitch()) * kRadToDeg;
    std::printf("\n=== residual forward-view pan ===\n");
    std::printf("intervals            %d   median matches/interval %.0f\n",
                est.intervalCount(), est.medianMatches());
    std::printf("raw pan RMS          %.4f / %.4f deg/frame (yaw/pitch)\n", rawYawRms, rawPitchRms);
    std::printf("correction RMS       %.4f / %.4f deg  (strength %.2f, window %.2f s half)\n",
                rms(track.dyaw) * kRadToDeg, rms(track.dpitch) * kRadToDeg, a.residStrength,
                insv::leanWindowFromStrength(a.residStrength));
    std::printf("correction max |.|   %.3f / %.3f deg\n", maxAbs(track.dyaw) * kRadToDeg,
                maxAbs(track.dpitch) * kRadToDeg);

    // Gyro-correction roll/pitch over the range -- the old Euler HorizonLock
    // path drifted / blew toward +-180; the fixed one should stay bounded and
    // smooth (roll tracks and cancels the camera's roll).
    if (gyro.ready()) {
        std::vector<float> corrRoll, corrPitch;
        for (const double ts : est.times()) {
            float m[3][3], y, p, r;
            gyro.correctionMatrix(ts, m);
            insv::MatrixToYawPitchRoll(m, y, p, r);
            corrRoll.push_back(r);
            corrPitch.push_back(p);
        }
        auto meanFirstLast = [](const std::vector<float>& v, int k, bool last) {
            double s = 0;
            int n = 0;
            for (int i = 0; i < k && i < (int)v.size(); ++i) {
                s += last ? v[v.size() - 1 - i] : v[i];
                ++n;
            }
            return n ? s / n : 0.0;
        };
        const int k = std::max(1, static_cast<int>(std::lround(fps)));
        const double driftRoll =
            (meanFirstLast(corrRoll, k, true) - meanFirstLast(corrRoll, k, false)) * kRadToDeg;
        std::printf("\n=== gyro correction (%s) ===\n", a.stabMode.c_str());
        std::printf("roll  RMS %.3f  range [%.2f, %.2f]  drift %.3f deg\n", rms(corrRoll) * kRadToDeg,
                    *std::min_element(corrRoll.begin(), corrRoll.end()) * kRadToDeg,
                    *std::max_element(corrRoll.begin(), corrRoll.end()) * kRadToDeg, driftRoll);
        std::printf("pitch RMS %.3f  range [%.2f, %.2f] deg\n", rms(corrPitch) * kRadToDeg,
                    *std::min_element(corrPitch.begin(), corrPitch.end()) * kRadToDeg,
                    *std::max_element(corrPitch.begin(), corrPitch.end()) * kRadToDeg);
    }

    // --- diagnostic blocks [3] and [4] (need the analysed image) ----------
    if (a.diagnose) {
        insv::StabConfig diagCfg =
            gcfg.mode != insv::StabMode::Off
                ? gcfg
                : withAxes(insv::StabConfig::preset(insv::StabMode::LeanFollow));
        diagCfg.timeOffsetSec = gyro.effectiveTimeOffsetSec();
        if (imu)
            diagImageVsGyro(est, imu->samples, diagCfg, fps, a);
        else
            std::printf("\n=== [3] skipped -- no IMU track ===\n");

        if (imu && estRaw && estRaw->intervalCount() >= 2)
            diagVideoImuSync(*estRaw, imu->samples, exposure, fps, diagCfg, a);
        else
            std::printf("\n=== [5] skipped -- no IMU / raw-image track ===\n");

        std::printf("\n=== [4] forward narrow vs. wide residual (rotation vs. parallax) ===\n");
        if (estWide && estWide->intervalCount() >= 2) {
            const double nY = rmsFirstDiff(est.accumulatedYaw()) * kRadToDeg;
            const double nP = rmsFirstDiff(est.accumulatedPitch()) * kRadToDeg;
            const double wY = rmsFirstDiff(estWide->accumulatedYaw()) * kRadToDeg;
            const double wP = rmsFirstDiff(estWide->accumulatedPitch()) * kRadToDeg;
            const double nMag = std::sqrt(nY * nY + nP * nP);
            const double wMag = std::sqrt(wY * wY + wP * wP);
            std::printf("narrow forward cone  residual %.4f deg/frame  (%.0f feat/intvl)\n", nMag,
                        est.medianMatches());
            std::printf("wide forward cone    residual %.4f deg/frame  (%.0f feat/intvl)\n", wMag,
                        estWide->medianMatches());
            std::printf("wide/narrow ratio    %.2f\n", nMag > 1e-9 ? wMag / nMag : 0.0);
            std::printf("  (~1 => de-rotation works evenly, shake is far-field rotation, "
                        "correctable.\n"
                        "   >>1 => the wider cone's nearer content still moves after "
                        "de-rotation =>\n"
                        "   translational parallax, which sphere rotation cannot remove.)\n");
        } else {
            std::printf("  (wide-cone estimator produced too few intervals)\n");
        }
    }

    if (a.render == "off") {
        if (!a.diagnose)
            std::printf("\n(measure-only; --render derotate2d + out.mp4 to eyeball)\n");
        return 0;
    }

    // --- render pass: bake gyro (+ residual) into the pose, GPU reproject ---
    int eqW = a.renderEqW > 0 ? a.renderEqW : std::max(3840, 2 * a.outW);
    eqW &= ~1;
    const int eqH = eqW / 2;
    const float fovY = static_cast<float>(a.fovDeg * kDegToRad);

    stitch::PreviewStitchConfig rsc;
    rsc.width = eqW;
    rsc.height = eqH;
    auto rsrc = std::make_unique<stitch::NativeStitchSource>();
    if (!rsrc->Open(a.input, rsc) || !rsrc->SeekTo(trimIn))
        die(rsrc->GetLastError());
    // Stitch capture timing (the app does the same whenever stabilization is
    // on): every sensor row brought to the stab's instant over the clip's
    // readout; --rs-stitch / --lens-offset override it for experiments.
    // Camera motion from a pure-gyro track, independent of --stab-mode.
    insv::GyroStabilizer rsGyro;
    if (imu && (a.rsStitchMs[0] != 0.0f || a.rsStitchMs[1] != 0.0f || !a.noCaptureTiming)) {
        insv::StabConfig rc = insv::StabConfig::preset(insv::StabMode::LeanFollow);
        rc.timeOffsetSec = a.timeOffsetMs / 1000.0f; // same sync as the stab (0 = the file's)
        rsGyro.build(*imu, rc, a.syncAdjustMs / 1000.0);
        stitch::CaptureTiming ct;
        if (a.haveLensOffset) {
            ct.lensOffsetSec[0] = a.lensOffsetMs[0] / 1000.0;
            ct.lensOffsetSec[1] = a.lensOffsetMs[1] / 1000.0;
        }
        // Per-row readout: --rs-stitch, else the stabilizer's model (the app's).
        const bool rsGiven = a.rsStitchMs[0] != 0.0f || a.rsStitchMs[1] != 0.0f;
        ct.readoutSec[0] = rsGiven ? a.rsStitchMs[0] / 1000.0 : a.noCaptureTiming ? 0.0 : rsGyro.readoutSec();
        ct.readoutSec[1] = rsGiven ? a.rsStitchMs[1] / 1000.0 : a.noCaptureTiming ? 0.0 : rsGyro.readoutSec();
        ct.scanColumns = a.rsCols;
        ct.rotationAt = [&rsGyro](double t, double dt, float r[3]) { rsGyro.bodyRotation(t, dt, r); };
        rsrc->SetCaptureTiming(ct);
        std::printf("stitch capture timing: lens offsets %.2f / %.2f ms, readout %.2f / %.2f ms "
                    "along fisheye %s\n",
                    ct.lensOffsetSec[0] * 1e3, ct.lensOffsetSec[1] * 1e3, ct.readoutSec[0] * 1e3,
                    ct.readoutSec[1] * 1e3, a.rsCols ? "columns" : "rows");
    }

    exportpipe::FlattenConfig fc;
    fc.out_path = a.output;
    fc.out_w = a.outW;
    fc.out_h = a.outH;
    fc.fps = fps;
    fc.h265 = a.h265;
    fc.bitrate_kbps = a.bitrateKbps;
    exportpipe::FlattenWriter writer;
    if (!writer.open(fc, err))
        die(err);

    const bool gpuFlat = rsrc->GpuReframeAvailable() && (a.outW % 4) == 0 && (a.outH % 2) == 0;
    std::printf("\nrender %s -> %s  (%s reproject)\n", a.render.c_str(), a.output.c_str(),
                gpuFlat ? "GPU" : "CPU");

    // Pose + stab for a decoded frame at its own source time (the stitch
    // hands us the pts -- baking by output index would be up to a frame off,
    // see NativeStitchSource::ReadFlatFrame), plus the residual yaw/pitch for
    // derotate2d. The gyro de-rotation rides as a sphere-frame matrix applied
    // after the pose (the app's composition); the legacy --stab-compose view
    // folds it into the pose instead (base * corr).
    const int64_t frameCount = static_cast<int64_t>(std::llround((trimOut - trimIn) / 1000.0 * fps));
    float base[3][3];
    insv::YawPitchRollToMatrix(static_cast<float>(a.baseYawDeg * kDegToRad),
                               static_cast<float>(a.basePitchDeg * kDegToRad),
                               static_cast<float>(a.baseRollDeg * kDegToRad), base);
    // --stab-in: lines of "tSec m00 m01 ... m22"; the nearest time is used.
    std::vector<double> stabInT;
    std::vector<std::array<float, 9>> stabInM;
    if (!a.stabIn.empty()) {
        FILE* f = std::fopen(a.stabIn.c_str(), "r");
        if (!f)
            die("could not open --stab-in");
        double t;
        std::array<float, 9> m;
        while (std::fscanf(f, "%lf", &t) == 1) {
            for (float& v : m)
                if (std::fscanf(f, "%f", &v) != 1)
                    die("--stab-in: short line");
            stabInT.push_back(t);
            stabInM.push_back(m);
        }
        std::fclose(f);
        if (stabInT.empty())
            die("--stab-in: no matrices");
    }
    auto poseAt = [&](int64_t ptsMs, view::Pose &pose, float stab[9]) {
        const double tSec = ptsMs / 1000.0;
        float corr[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        if (!stabInT.empty()) {
            const size_t k = std::lower_bound(stabInT.begin(), stabInT.end(), tSec) - stabInT.begin();
            size_t best = std::min(k, stabInT.size() - 1);
            if (k > 0 && std::fabs(stabInT[k - 1] - tSec) < std::fabs(stabInT[best] - tSec))
                best = k - 1;
            std::copy(stabInM[best].begin(), stabInM[best].end(), &corr[0][0]);
        } else if (a.render != "none" && gyro.ready()) {
            gyro.correctionMatrix(tSec, corr);
        }
        float y = 0.0f, p = 0.0f, r = 0.0f;
        float m[3][3];
        if (a.stabInSphere) {
            std::copy(&base[0][0], &base[0][0] + 9, &m[0][0]);
            std::copy(&corr[0][0], &corr[0][0] + 9, stab);
        } else {
            insv::MatrixMul(base, corr, m); // base * correction, prepended to the ray
            const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            std::copy(ident, ident + 9, stab);
        }
        insv::MatrixToYawPitchRoll(m, y, p, r);
        if (a.render == "derotate2d") {
            float dY = 0.0f, dP = 0.0f;
            track.sample(tSec, dY, dP);
            y += dY;
            p += dP;
        }
        pose.yaw = y;
        pose.pitch = p;
        pose.roll = r;
        pose.fov = view::fovFor<double>(fovY, 0.0, double(a.outW) / double(a.outH)); // rectilinear
        pose.distortion = 0.0f;
    };

    int64_t written = 0;
    int lastPct = -1;
    while (written < frameCount) {
        if (gpuFlat) {
            stitch::FlatFrame ff = rsrc->ReadFlatFrame(a.outW, a.outH, poseAt);
            if (ff.data.empty()) {
                if (!rsrc->GetLastError().empty())
                    die(rsrc->GetLastError());
                break;
            }
            if (ff.timestamp_ms + halfFrameMs < trimIn)
                continue;
            if (!writer.addNv12Frame(ff.data.data(), ff.width, ff.height, err))
                die(err);
        } else {
            stitch::Frame eq = rsrc->ReadFrame();
            if (eq.rgba.empty()) {
                if (!rsrc->GetLastError().empty())
                    die(rsrc->GetLastError());
                break;
            }
            if (eq.timestamp_ms + halfFrameMs < trimIn)
                continue;
            view::Pose pose;
            float stab[9];
            poseAt(eq.timestamp_ms, pose, stab);
            if (!writer.addEquirectFrame(eq.rgba.data(), eq.width, eq.height, &pose, stab, nullptr, err))
                die(err);
        }
        ++written;
        const int pct = frameCount > 0 ? static_cast<int>(written * 100 / frameCount) : 100;
        if (pct != lastPct) {
            lastPct = pct;
            std::printf("\r  render %3d%%", pct);
            std::fflush(stdout);
        }
    }
    std::printf("\n");

    const int64_t total = writer.finish(err);
    if (total < 0)
        die(err);
    std::printf("wrote %lld frames -> %s\n", static_cast<long long>(total), a.output.c_str());
    return 0;
}
