// Standalone checks for the cleanroom stitch core (src/insv/): calibration
// string parsing, lens profiles, the linear-algebra helpers, and
// FisheyeStitcher geometry, the trailer's thumbnail record, the lens streams'
// Y'CbCr conversion. All synthetic -- no
// .insv file, no FFmpeg, no Qt -- so it links only the dependency-free `insv`
// and `insv_trailer` libs and runs anywhere. Exit code
// is the failure count. Mirrors the model tests' harness (tests/support/ModelTest.h).

#include "BlurQuads.h"
#include "Calibration.h"
#include "FisheyeProjection.h"
#include "Image.h"
#include "InsvModels.h"
#include "InsvTrailer.h"
#include "SeamAlignment.h"
#include "SeamDepth.h"
#include "Ycbcr.h"
#include "support/TestTrailer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

using testtrailer::Bytes;
using testtrailer::TrailerRecord;
using testtrailer::insvTrailer;

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// The short "Parameters" maker-note string from the X5 sample clips -- not
// parsed any more; only used to check the extended parser rejects it.
const char* kX5Params =
    "2 2663.208 2705.380 2687.820 0.326 -0.017 89.997 "
    "2648.963 8071.630 2687.090 -0.283 -0.065 88.577 10752 5376 1137";

// The exact extended MEI calibration string from the X5 sample clip
// VID_20250904_193105 (plaintext in the .insv trailer / the .lrv.pb sidecar).
const char* kX5Extended =
    "2_2.000000_4294.030_4293.860_2703.310_2684.030_0.241_0.068_89.993_"
    "0.000000_0.000000_0.000000_"
    "0.22969893_1.60199785_-1.24233508_-3.16975212_0.00000000_"
    "-0.00144594_0.00040147_-0.00395632_-0.00326463_0.00195414_0.00044626_"
    "0.01116405_0.00953672_10752_5376_113_"
    "2.000000_4277.660_4277.270_8073.210_2692.040_-0.088_-0.141_88.580_"
    "0.000774_0.000174_-0.031796_"
    "0.24695949_1.25187707_0.26544470_-5.21495152_0.00000000_"
    "-0.00144929_-0.00059055_0.00577204_0.01037207_-0.00314844_-0.00378038_"
    "-0.00842754_-0.02442369_10752_5376_113_394240";

void testExtendedCalibration()
{
    insv::InsvCalibration c;
    CHECK(insv::ParseExtendedCalibrationString(kX5Extended, c));
    CHECK(c.lensesExt.size() == 2);
    if (c.lensesExt.size() != 2)
        return;
    const auto& e0 = c.lensesExt[0];
    const auto& e1 = c.lensesExt[1];
    CHECK(near(e0.xi, 2.0, 1e-9));
    CHECK(near(e0.fx, 4294.030, 1e-3) && near(e0.fy, 4293.860, 1e-3));
    CHECK(near(e0.cx, 2703.310, 1e-3) && near(e0.cy, 2684.030, 1e-3));
    CHECK(near(e0.yaw, 0.241, 1e-3) && near(e0.pitch, 0.068, 1e-3));
    CHECK(near(e0.k[0], 0.22969893, 1e-6) && near(e0.k[3], -3.16975212, 1e-6));
    CHECK(near(e0.p[0], -0.00144594, 1e-6) && near(e0.s[0], -0.00395632, 1e-6));
    CHECK(near(e0.w, 10752.0, 1e-6) && near(e0.h, 5376.0, 1e-6));
    CHECK(near(e1.cx, 8073.210, 1e-3)); // back lens: offset in the side-by-side frame
    CHECK(near(e1.k[0], 0.24695949, 1e-6));

    // Tolerates the string embedded in a longer buffer (as it sits in a
    // protobuf field in the trailer, between binary framing bytes).
    insv::InsvCalibration c2;
    const std::string embedded =
        std::string("\x01\x02\x03prefix", 9) + kX5Extended + std::string("\x00\x04end", 5);
    CHECK(insv::ParseExtendedCalibrationString(embedded, c2));
    CHECK(c2.lensesExt.size() == 2);

    // Rejects strings that aren't the 56-token extended form; leaves `out` empty.
    insv::InsvCalibration c3;
    CHECK(!insv::ParseExtendedCalibrationString(kX5Params, c3)); // short "p2_" form
    CHECK(c3.lensesExt.empty());
    CHECK(!insv::ParseExtendedCalibrationString("2_1_2_3", c3));
    CHECK(!insv::ParseExtendedCalibrationString("", c3));
    CHECK(!insv::ParseExtendedCalibrationString("2_2.0_junk", c3));
    CHECK(c3.lensesExt.empty());
}

// The ONE X2's 40-token offset_v3 (metadata field 54) from the sample clip
// VID_20250816_103807_00_001 (docs/formats/insta360-one-x2.md §7).
const char* kOneX2V3 =
    "2_1.728590_2322.610_2322.410_1528.430_1521.450_0.799_-0.157_178.977_"
    "0.000000_0.000000_0.000000_0.22791584_-0.14894706_-0.96843737_"
    "-0.00185287_0.00115803_6080_3040_41_"
    "1.728590_2318.480_2318.720_4554.650_1542.810_-0.848_-0.368_1.421_"
    "0.000174_-0.001947_-0.021072_0.24361213_-0.22786462_-0.84430271_"
    "0.00129106_-0.00041729_6080_3040_41_199680";

void testCalibrationV3()
{
    std::printf("  offset_v3 calibration\n");
    insv::InsvCalibration c;
    CHECK(insv::ParseCalibrationV3String(kOneX2V3, c));
    CHECK(c.lensesExt.size() == 2);
    if (c.lensesExt.size() != 2)
        return;
    const auto& e0 = c.lensesExt[0];
    const auto& e1 = c.lensesExt[1];
    CHECK(near(e0.xi, 1.72859, 1e-9));
    CHECK(near(e0.fx, 2322.610, 1e-3) && near(e0.fy, 2322.410, 1e-3));
    CHECK(near(e0.cx, 1528.430, 1e-3) && near(e0.cy, 1521.450, 1e-3));
    CHECK(near(e0.roll, 178.977, 1e-3));
    CHECK(near(e0.k[0], 0.22791584, 1e-9) && near(e0.k[2], -0.96843737, 1e-9));
    CHECK(e0.k[3] == 0.0 && e0.s[0] == 0.0 && e0.s[3] == 0.0); // not in this form
    CHECK(near(e0.p[0], -0.00185287, 1e-9) && near(e0.p[1], 0.00115803, 1e-9));
    CHECK(e0.w == 6080.0 && e0.h == 3040.0 && e0.type == 41);
    CHECK(near(e1.cx, 4554.650, 1e-3) && near(e1.tz, -0.021072, 1e-9));
    CHECK(near(e1.p[1], -0.00041729, 1e-9));

    // Too short, or not a calibration at all: nothing, `out` untouched.
    insv::InsvCalibration bad;
    CHECK(!insv::ParseCalibrationV3String(kX5Params, bad)); // 16 tokens
    CHECK(!insv::ParseCalibrationV3String("", bad));
    CHECK(bad.lensesExt.empty());

    // The stitch takes it as it takes the X5's: fx in field 27's window
    // (2880 on the ONE X2), cx / cy in the 3040-wide lens half.
    for (auto& e : c.lensesExt)
        e.focalRefW = 2880.0;
    insv::FisheyeStitcher st;
    CHECK(st.configure(c, 2880, 2880, insv::StitchParams{}));
}

// The X5 calibration as the reader hands it over: with field 27's focal
// reference, which the string doesn't carry.
insv::InsvCalibration x5Calibration()
{
    insv::InsvCalibration c;
    CHECK(insv::ParseExtendedCalibrationString(kX5Extended, c));
    for (insv::LensCalibExt &e : c.lensesExt)
        e.focalRefW = 5312.0;
    return c;
}

void testMeiProjection()
{
    insv::InsvCalibration c = x5Calibration();

    // fx / fy are in field 27's sensor window: no reference, no stitch; the
    // X6's larger window gives a shorter focal length in video pixels.
    {
        insv::InsvCalibration noRef = c;
        noRef.lensesExt[1].focalRefW = 0;
        insv::FisheyeStitcher st;
        CHECK(!st.configure(noRef, 3840, 3840, insv::StitchParams{}));
        insv::InsvCalibration wide = c;
        for (insv::LensCalibExt &e : wide.lensesExt)
            e.focalRefW = 7680.0;
        insv::FisheyeStitcher a, b;
        CHECK(a.configure(c, 3840, 3840, insv::StitchParams{}));
        CHECK(b.configure(wide, 3840, 3840, insv::StitchParams{}));
        CHECK(near(b.lensProjection(0).fx, a.lensProjection(0).fx * 5312.0 / 7680.0, 1e-9));
    }

    // configure() needs the extended calibration.
    insv::FisheyeStitcher none;
    CHECK(!none.configure(insv::InsvCalibration{}, 3840, 3840, insv::StitchParams{}));
    CHECK(!none.ready());

    insv::FisheyeStitcher st;
    CHECK(st.configure(c, 3840, 3840, insv::StitchParams{}));
    CHECK(st.ready());

    const insv::LensProjection& l0 = st.lensProjection(0);
    // cx/cy scaled sensor(5376) -> video(3840): 2703.31 * 3840/5376.
    CHECK(near(l0.cx, 2703.310 * 3840.0 / 5376.0, 1e-2));
    CHECK(near(l0.cy, 2684.030 * 3840.0 / 5376.0, 1e-2));
    // fx scaled by 3840/5312.
    CHECK(near(l0.fx, 4294.030 * 3840.0 / 5312.0, 1e-2));

    // World +Z is within the lens's small (~0.24 deg) extrinsic tilt of its
    // optical axis, so it must project very close to the principal point.
    double u, v, theta;
    CHECK(st.project(0, insv::Vec3{0, 0, 1}, u, v, theta));
    CHECK(near(theta, 0.0, 0.02)); // rad; extrinsic yaw/pitch ~ 0.25 deg
    CHECK(near(u, l0.cx, 15.0) && near(v, l0.cy, 15.0));
    // The lens's own optical axis (worldToLens^T * +Z) projects to the
    // principal point to sub-pixel.
    const insv::Mat3& w2l = l0.worldToLens;
    const insv::Vec3 axis{w2l.m[2][0], w2l.m[2][1], w2l.m[2][2]}; // 3rd row = w2l^T col 2
    CHECK(st.project(0, axis, u, v, theta));
    CHECK(near(theta, 0.0, 1e-6));
    CHECK(near(u, l0.cx, 1e-3) && near(v, l0.cy, 1e-3));

    // radiusForTheta is monotonic and ~1 at thetaRef under MEI.
    double prev = -1;
    for (double deg = 0; deg <= 95; deg += 5) {
        const double r = st.radiusForTheta(deg * kDeg);
        CHECK(std::isfinite(r) && r >= prev - 1e-9);
        prev = r;
    }
    CHECK(near(st.radiusForTheta(100 * kDeg), 1.0, 1e-6));

    // An off-axis ray lands a sane distance from the centre (inside the frame).
    CHECK(st.project(0, insv::normalize(insv::Vec3{0.5, 0.0, 1.0}), u, v, theta));
    const double rad = std::hypot(u - l0.cx, v - l0.cy);
    CHECK(rad > 10.0 && rad < 3840.0);

    // Directly behind lens 0's axis (theta = 180) is past the cutoff.
    const insv::Vec3 behind{-axis.x, -axis.y, -axis.z};
    CHECK(!st.project(0, behind, u, v, theta));
}

// The X5's field-145 lens profiles (theta deg -> mm) as read from the sample
// footage.
insv::InsvMakerNotes x5LensProfiles()
{
    insv::InsvMakerNotes n;
    n.lens_profiles.push_back({"bare", {0, 0.03159, -8.415e-05, 2.201e-06, -1.284e-08, 0}, 200});
    n.lens_profiles.push_back(
        {"ProtectorA", {0, 0.0321906505, -9.37245e-05, 2.5248e-06, -1.5e-08, 0}, 195});
    return n;
}

void testLensProfile()
{
    const insv::InsvMakerNotes notes = x5LensProfiles();
    CHECK(insv::FindLensProfile(notes, "ProtectorA") != nullptr);
    CHECK(insv::FindLensProfile(notes, "ProtectorX") == nullptr);

    const insv::LensProfile* bareProfile = insv::FindLensProfile(notes, "bare");
    insv::StitchParams bare;
    CHECK(insv::ApplyLensProfile(bare, bareProfile, nullptr));
    CHECK(!bare.accessory && near(bare.fovDeg, 200, 1e-9));

    insv::StitchParams guard;
    CHECK(insv::ApplyLensProfile(guard, bareProfile, insv::FindLensProfile(notes, "ProtectorA")));
    CHECK(guard.accessory && near(guard.fovDeg, 195, 1e-9));

    // An accessory profile without the bare one to scale against.
    insv::StitchParams missing;
    CHECK(!insv::ApplyLensProfile(missing, nullptr, insv::FindLensProfile(notes, "ProtectorA")));
    CHECK(!missing.accessory);

    insv::InsvCalibration c = x5Calibration();
    insv::FisheyeStitcher sb, sg;
    CHECK(sb.configure(c, 3840, 3840, bare));
    CHECK(sg.configure(c, 3840, 3840, guard));

    // The guard pushes the seam-angle image ~2.3 % further out (measured seam
    // fit: 2.0-2.5 %), and nothing at the centre.
    CHECK(near(sb.accessoryScale(92 * kDeg), 1.0, 1e-12));
    CHECK(near(sg.accessoryScale(92 * kDeg), 1.024, 0.002));
    CHECK(near(sg.accessoryScale(0.0), 0.0321906505 / 0.03159, 1e-3));

    const insv::LensProjection& l0 = sg.lensProjection(0);
    const insv::Mat3& w2l = l0.worldToLens;
    const insv::Mat3 l2w = insv::transpose(w2l);
    const insv::Vec3 at92 = insv::apply(l2w, insv::Vec3{std::sin(92 * kDeg), 0, std::cos(92 * kDeg)});
    double ub, vb, ug, vg, t;
    CHECK(sb.project(0, at92, ub, vb, t));
    CHECK(sg.project(0, at92, ug, vg, t));
    const double rb = std::hypot(ub - l0.cx, vb - l0.cy), rg = std::hypot(ug - l0.cx, vg - l0.cy);
    CHECK(near(rg / rb, 1.024, 0.003));

    // Edge normalisation follows the FOV: ~1 at 97.5 deg with guards.
    CHECK(near(sg.radiusForTheta(97.5 * kDeg), 1.0, 1e-6));
    // ...and the seam band ends inside it.
    const insv::StitchGpuParams gp = sg.gpuParams();
    CHECK(gp.accessory == 1);
    CHECK((gp.seamRad + gp.bandRad) / kDeg <= 97.5 - insv::StitchParams::kSeamMarginDeg + 1e-4);
}

void testLinearAlgebra()
{
    // rotations are orthonormal: transpose(R) * R == I
    for (double a : {0.3, -1.2, 2.7}) {
        for (const insv::Mat3& R : {insv::rotX(a), insv::rotY(a), insv::rotZ(a)}) {
            const insv::Mat3 I = insv::mul(insv::transpose(R), R);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    CHECK(near(I.m[i][j], i == j ? 1.0 : 0.0, 1e-9));
        }
    }
    // rotZ(90deg) maps +X -> +Y
    const insv::Vec3 x = insv::apply(insv::rotZ(kPi / 2), insv::Vec3{1, 0, 0});
    CHECK(near(x.x, 0.0, 1e-9) && near(x.y, 1.0, 1e-9) && near(x.z, 0.0, 1e-9));
    // mul is real composition: (Ry * Rx) applied == Ry(Rx v)
    const insv::Mat3 Ry = insv::rotY(0.7), Rx = insv::rotX(-0.4);
    const insv::Vec3 v{0.2, -0.5, 0.9};
    const insv::Vec3 a = insv::apply(insv::mul(Ry, Rx), v);
    const insv::Vec3 b = insv::apply(Ry, insv::apply(Rx, v));
    CHECK(near(a.x, b.x, 1e-9) && near(a.y, b.y, 1e-9) && near(a.z, b.z, 1e-9));
}

void testSyntheticRender()
{
    // lens 0 solid red, lens 1 solid blue.
    insv::Image l0, l1;
    l0.alloc(800, 800);
    l1.alloc(800, 800);
    for (size_t i = 0; i < l0.rgb.size(); i += 3) {
        l0.rgb[i] = 220; // R
        l1.rgb[i + 2] = 220; // B
    }

    insv::FisheyeStitcher st;
    insv::InsvCalibration c = x5Calibration();
    CHECK(st.configure(c, 800, 800, insv::StitchParams{}));

    insv::Image eq = st.renderEquirect(l0, l1, 512);
    CHECK(eq.w == 512 && eq.h == 256);

    auto px = [&](int x, int y) {
        const size_t o = (static_cast<size_t>(y) * eq.w + x) * 3;
        return std::array<int, 3>{eq.rgb[o], eq.rgb[o + 1], eq.rgb[o + 2]};
    };
    // With outYaw = 180, equirect lon = 0 (centre column) maps to world
    // -Z-then-flipped = lens 0 (red); lon = +-180 (edge) maps to lens 1 (blue).
    const auto centre = px(eq.w / 2, eq.h / 2);
    const auto edge = px(2, eq.h / 2);
    CHECK(centre[0] > centre[2]);         // centre is reddish
    CHECK(edge[2] > edge[0]);             // edge is bluish
    CHECK(centre[0] + centre[1] + centre[2] > 0); // not a black hole
}

} // namespace

// Depth-aware seam: the seam frame, lens centres, the q table lookup and
// the per-lens rays.
void testSeamDepthGeometry()
{
    insv::InsvCalibration c = x5Calibration();
    insv::StitchParams p;
    p.depthSeam = true;
    insv::FisheyeStitcher st;
    CHECK(st.configure(c, 3840, 3840, p));

    // Seam frame: orthonormal, the axis along lens 0's optical axis.
    const insv::Vec3 &a = st.seamAxis(), &u = st.seamU(), &v = st.seamV();
    auto dot = [](const insv::Vec3& x, const insv::Vec3& y) { return x.x * y.x + x.y * y.y + x.z * y.z; };
    CHECK(near(dot(a, a), 1.0, 1e-9) && near(dot(u, u), 1.0, 1e-9) && near(dot(v, v), 1.0, 1e-9));
    CHECK(near(dot(a, u), 0.0, 1e-9) && near(dot(a, v), 0.0, 1e-9) && near(dot(u, v), 0.0, 1e-9));
    const double* z0 = st.lensProjection(0).worldToLens.m[2];
    CHECK(dot(a, insv::Vec3{z0[0], z0[1], z0[2]}) > 0.999);

    // Lens 0 in front along its axis, lens 1 behind: |tz| = 31.8 mm apart.
    const insv::Vec3 c0 = st.lensCentre(0), c1 = st.lensCentre(1);
    CHECK(near(dot(c0, a), 0.031796 / 2, 1e-6) && near(dot(c1, a), -0.031796 / 2, 1e-6));

    // q = 0 leaves the ray alone; q > 0 pushes each lens's ray away from its
    // own axis, by ~ q * baseline / 2 rad on the seam.
    const insv::Vec3 d = st.seamDir(1.0, 0.0);
    const insv::Vec3 r0 = st.lensRay(0, d, 0.0);
    CHECK(near(r0.x, d.x, 1e-12) && near(r0.y, d.y, 1e-12) && near(r0.z, d.z, 1e-12));
    const double q = 2.0; // 0.5 m
    const insv::Vec3 s0 = st.lensRay(0, d, q), s1 = st.lensRay(1, d, q);
    CHECK(near(std::asin(dot(s0, a)), -q * 0.031796 / 2, 1e-4)); // towards lens 1's side
    CHECK(near(std::asin(dot(s1, a)), +q * 0.031796 / 2, 1e-4));

    // The table: linear in azimuth between bin centres, full inside the blend
    // band, faded to 0 past band + ramp; nothing without a table or when off.
    float table[insv::kSeamDepthBins];
    for (int b = 0; b < insv::kSeamDepthBins; ++b)
        table[b] = static_cast<float>(b);
    const double binRad = 2 * kPi / insv::kSeamDepthBins;
    CHECK(near(st.seamQAt(st.seamDir(10.5 * binRad, 0.0), table), 10.0, 1e-6));
    CHECK(near(st.seamQAt(st.seamDir(11.0 * binRad, 0.0), table), 10.5, 1e-6));
    CHECK(near(st.seamQAt(st.seamDir(0.0, 0.0), table), 0.5 * (127 + 0), 1e-6)); // wraps
    CHECK(near(st.seamQAt(st.seamDir(10.5 * binRad, 2 * kDeg), table), 10.0, 1e-6));
    CHECK(st.seamQAt(st.seamDir(10.5 * binRad, 40 * kDeg), table) == 0.0);
    const double mid = st.seamQAt(st.seamDir(10.5 * binRad, 15 * kDeg), table); // in the ramp
    CHECK(mid > 0.5 && mid < 9.5);
    CHECK(st.seamQAt(st.seamDir(10.5 * binRad, 0.0), nullptr) == 0.0);
    insv::FisheyeStitcher off;
    CHECK(off.configure(c, 3840, 3840, insv::StitchParams{}));
    CHECK(off.seamQAt(off.seamDir(10.5 * binRad, 0.0), table) == 0.0);

    // --baseline < 0 swaps the lenses.
    insv::StitchParams pf = p;
    pf.baselineM = -0.05;
    insv::FisheyeStitcher flipped;
    CHECK(flipped.configure(c, 3840, 3840, pf));
    CHECK(near(dot(flipped.lensCentre(0), flipped.seamAxis()), -0.025, 1e-9));
}

// SeamDepthTracker on a synthetic cost volume: a near object over one arc of
// bins, far elsewhere, and textureless bins that must follow their neighbours.
void testSeamDepthTracker()
{
    constexpr int B = insv::kSeamDepthBins, K = insv::kSeamDepthCands;
    auto costs = std::make_unique<insv::SeamCosts>();
    auto fill = [&](double nearQ) {
        for (int b = 0; b < B; ++b) {
            const double truth = (b >= 40 && b < 70) ? nearQ : 0.0;
            const bool textured = !(b == 55 || b == 100);
            for (int k = 0; k < K; ++k) {
                const double dq = insv::SeamCandQ(k) - truth;
                costs->ncc[b][k] = static_cast<float>(textured ? 0.95 - 0.8 * std::min(std::fabs(dq), 1.0) : 0.0);
                costs->texture[b][k] = textured ? 30.0f : 0.5f;
            }
        }
    };
    insv::SeamDepthTracker tr;
    fill(1.8);
    tr.update(*costs, 1000);
    const float* q = tr.table();
    CHECK(tr.hasHistory());
    CHECK(near(q[50], 1.8, 0.1) && near(q[60], 1.8, 0.1));
    CHECK(near(q[55], 1.8, 0.15));                    // textureless: from its neighbours
    CHECK(near(q[10], 0.0, 0.1) && near(q[100], 0.0, 0.1) && near(q[120], 0.0, 0.1));
    CHECK(q[40 - 3] < 0.3 && q[70 + 2] < 0.3);         // the edge stays an edge

    // Next frame, the object moved closer: the table follows, partly (blend).
    fill(2.4);
    tr.update(*costs, 1033);
    CHECK(q[60] > 1.9 && q[60] < 2.45);
    // A jump in time starts over: no blend with the old table.
    tr.update(*costs, 5000);
    CHECK(near(q[60], 2.4, 0.1));
    tr.reset();
    CHECK(!tr.hasHistory() && tr.table()[60] == 0.0f);
}

// Privacy blur quads: (s, t) <-> direction round trip, corners, rejects, cells.
// Seam alignment fine-tune on synthetic lens pictures: a textured sphere
// seen through a stitch with a known correction, from which the estimator,
// starting at none, has to find its way back -- with a stretch of near
// content (parallax) that it must not take for the calibration.
namespace {

// Grey texture on the sphere: plane waves of ~1.5-2.5 deg wavelength in
// scattered directions, so every block matches in one place only.
double seamTexture(const insv::Vec3 &d, int seed)
{
    double v = 128.0;
    for (int j = 0; j < 7; ++j) {
        const double t = 1.7 * j + 0.9 * seed, f = 150.0 + 17.0 * ((j * 5 + seed) % 7);
        const insv::Vec3 k{std::cos(t) * std::sin(1.1 * t + 0.4), std::sin(t) * std::sin(1.1 * t + 0.4),
                           std::cos(1.1 * t + 0.4)};
        v += 15.0 * std::sin(f * (k.x * d.x + k.y * d.y + k.z * d.z) + 0.7 * j + seed);
    }
    return std::clamp(v, 0.0, 255.0);
}

// Lens i's picture around the seam (78..102 deg off lens 0's axis) through
// `st`. Between azimuths nearFrom..nearTo (deg) the scene is 1/nearQ m away,
// so each lens sees it from its own centre.
insv::Image seamLensPicture(const insv::FisheyeStitcher &st, int lens, int size, int seed,
                            double nearFrom, double nearTo, double nearQ)
{
    insv::Image im;
    im.alloc(size, size);
    const insv::Mat3 l2w = insv::transpose(st.lensProjection(0).worldToLens);
    const insv::Vec3 c = st.lensCentre(lens);
    constexpr double step = 0.05;
    for (double th = 78.0; th <= 102.0; th += step)
        for (double a = 0.0; a < 360.0; a += step) {
            const insv::Vec3 d = insv::apply(
                l2w, {std::cos(a * kDeg) * std::sin(th * kDeg), std::sin(a * kDeg) * std::sin(th * kDeg),
                      std::cos(th * kDeg)});
            insv::Vec3 scene = d;
            if (a >= nearFrom && a < nearTo) {
                // The point on the sphere of radius 1/q that lens `lens` sees
                // along d: |c + s d| = 1/q.
                const double R = 1.0 / nearQ, cd = c.x * d.x + c.y * d.y + c.z * d.z;
                const double cc = c.x * c.x + c.y * c.y + c.z * c.z;
                const double s = -cd + std::sqrt(cd * cd - cc + R * R);
                scene = insv::normalize({c.x + s * d.x, c.y + s * d.y, c.z + s * d.z});
            }
            double u, v, t;
            if (!st.project(lens, d, u, v, t))
                continue;
            const int x = static_cast<int>(std::lround(u)), y = static_cast<int>(std::lround(v));
            if (x < 0 || y < 0 || x >= size || y >= size)
                continue;
            const auto g = static_cast<uint8_t>(std::lround(seamTexture(scene, seed)));
            uint8_t *p = &im.rgb[(static_cast<size_t>(y) * size + x) * 3];
            p[0] = p[1] = p[2] = g;
        }
    return im;
}

} // namespace

void testSeamAlignment()
{
    const insv::InsvCalibration cal = x5Calibration();
    constexpr int kSize = 1536;
    insv::StitchParams truthParams;
    truthParams.seam = {{0.3, -0.4, 0.15}, {0.002, -0.0015}, 1.0015};
    insv::FisheyeStitcher truth;
    CHECK(truth.configure(cal, kSize, kSize, truthParams));

    // Far everywhere but 140..220 deg, 3 m away there.
    std::vector<insv::SeamFrame> frames;
    for (int seed = 0; seed < 2; ++seed)
        frames.push_back(insv::MakeSeamFrame(
            seamLensPicture(truth, 0, kSize, seed, 140.0, 220.0, 1.0 / 3.0),
            seamLensPicture(truth, 1, kSize, seed, 140.0, 220.0, 1.0 / 3.0)));

    const insv::StitchParams none;
    const insv::SeamAlignmentResult r = insv::EstimateSeamAlignment(cal, none, kSize, kSize, frames);
    std::printf("seam alignment: %s\n", r.message.c_str());
    CHECK(r.ok);
    CHECK(r.offsetBeforeDeg > 0.3 && r.offsetAfterDeg < 0.04);
    // Roll and the scale are pinned down; a tilt and the principal point's
    // shift across the seam look alike there, so only their sum is.
    CHECK(near(r.correction.lens1RotDeg[2], 0.15, 0.03));
    CHECK(near(r.correction.radialScale, 1.0015, 0.0003));

    // The correction applied, the lenses agree on far content.
    insv::StitchParams fitted;
    fitted.seam = r.correction;
    CHECK(insv::MeasureSeamOffset(cal, fitted, kSize, kSize, frames) < 0.04);
    CHECK(insv::MeasureSeamOffset(cal, none, kSize, kSize, frames) > 0.3);
}

void testBlurQuads()
{
    auto dirOf = [](double lonDeg, double latDeg, float out[3]) {
        const double lon = lonDeg * std::numbers::pi / 180.0;
        const double lat = latDeg * std::numbers::pi / 180.0;
        out[0] = static_cast<float>(std::cos(lat) * std::sin(lon));
        out[1] = static_cast<float>(std::sin(lat));
        out[2] = static_cast<float>(-std::cos(lat) * std::cos(lon));
    };
    // A plate-sized (2 x 0.5 deg) quad, a bit to the right and down, slightly
    // skewed: TL, TR, BR, BL.
    insv::BlurQuad q;
    dirOf(30.0, -10.0, q.corner[0]);
    dirOf(32.0, -10.1, q.corner[1]);
    dirOf(32.1, -10.6, q.corner[2]);
    dirOf(30.0, -10.5, q.corner[3]);
    insv::BlurQuadFrame f;
    CHECK(insv::prepareBlurQuad(q, f));
    // ~4:1 -> 5 cells on the short side, ~20 along the long one.
    CHECK(f.cells[1] == insv::kBlurCellsShort);
    CHECK(f.cells[0] >= 18 && f.cells[0] <= 22);

    const double st[][2] = {{0.5, 0.5}, {0.1, 0.9}, {0.95, 0.02}, {0.3, 0.7}};
    for (const auto& p : st) {
        double d[3], s = -1, t = -1;
        insv::blurQuadDir(f, p[0], p[1], d);
        CHECK(insv::blurQuadST(f, d, s, t));
        CHECK(near(s, p[0], 1e-5) && near(t, p[1], 1e-5));
    }
    // The corners map to the unit square's corners.
    const double cst[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k = 0; k < 4; ++k) {
        double d[3];
        insv::blurQuadDir(f, cst[k][0], cst[k][1], d);
        CHECK(near(d[0], q.corner[k][0], 1e-6) && near(d[1], q.corner[k][1], 1e-6) &&
              near(d[2], q.corner[k][2], 1e-6));
    }
    // Outside: beside it, and the antipode.
    float out[3];
    double s, t;
    dirOf(29.0, -10.2, out);
    const double od[3] = {out[0], out[1], out[2]};
    CHECK(!insv::blurQuadST(f, od, s, t));
    const double anti[3] = {-f.c[0], -f.c[1], -f.c[2]};
    CHECK(!insv::blurQuadST(f, anti, s, t));
    // Feather: full inside, zero on the edge.
    CHECK(near(insv::blurQuadWeight(0.5, 0.5), 1.0, 1e-12));
    CHECK(near(insv::blurQuadWeight(0.0, 0.5), 0.0, 1e-12));
    // Every tap of a cell lies in that cell.
    for (int i = 0; i < insv::kBlurTaps; ++i)
        for (int j = 0; j < insv::kBlurTaps; ++j) {
            double d[3], ts, tt;
            insv::blurTapDir(f, 0.52, 0.47, i, j, d);
            CHECK(insv::blurQuadST(f, d, ts, tt));
            CHECK(std::floor(ts * f.cells[0]) == std::floor(0.52 * f.cells[0]));
            CHECK(std::floor(tt * f.cells[1]) == std::floor(0.47 * f.cells[1]));
        }
    // Degenerate: all corners the same.
    insv::BlurQuad z;
    for (auto& c : z.corner)
        dirOf(10.0, 0.0, c);
    CHECK(!insv::prepareBlurQuad(z, f));
}

// A file that ends in a minimal Insta360 trailer holding one record:
// `payload` with id `id`, right before the 78-byte footer (sequential layout).
std::string writeTrailerFile(const char* name, uint16_t id, const std::vector<uint8_t>& payload)
{
    auto put32 = [](uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
    };
    std::vector<uint8_t> footer(78, 0);
    footer[0] = uint8_t(id);
    footer[1] = uint8_t(id >> 8);
    put32(&footer[2], uint32_t(payload.size()));
    put32(&footer[38], uint32_t(payload.size() + footer.size()));
    std::memcpy(&footer[78 - 32], "8db42d694ccc418790edff439fe026bf", 32);
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream f(path, std::ios::binary);
    const std::vector<uint8_t> video(64, 0x11); // stands in for the MP4 boxes
    f.write(reinterpret_cast<const char*>(video.data()), std::streamsize(video.size()));
    f.write(reinterpret_cast<const char*>(payload.data()), std::streamsize(payload.size()));
    f.write(reinterpret_cast<const char*>(footer.data()), std::streamsize(footer.size()));
    return path;
}

void testTrailerThumbnail()
{
    // The X5's layout, shrunk: 40-byte header, then an 8x4 NV12 picture.
    const int w = 8, h = 4;
    std::vector<uint8_t> rec(40 + w * h * 3 / 2);
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) rec[at + i] = uint8_t(v >> (8 * i));
    };
    put32(0, 1);
    put32(4, uint32_t(rec.size()));
    put32(8, 1);
    put32(16, w);
    put32(20, h);
    for (size_t i = 40; i < rec.size(); ++i) rec[i] = uint8_t(i);

    const std::string ok = writeTrailerFile("insv_test_thumb.insv", 0x200, rec);
    const auto thumb = insv::ReadInsvThumbnail(ok);
    CHECK(thumb.has_value());
    if (thumb) {
        CHECK(thumb->width == w && thumb->height == h);
        CHECK(thumb->nv12.size() == size_t(w * h * 3 / 2));
        CHECK(!thumb->nv12.empty() && thumb->nv12.front() == 40 && thumb->nv12.back() == uint8_t(rec.size() - 1));
    }
    std::filesystem::remove(ok);

    // A size that doesn't match the header (another model's JPEG), no 0x200
    // record, no trailer at all: nothing.
    rec.pop_back();
    const std::string bad = writeTrailerFile("insv_test_thumb_bad.insv", 0x200, rec);
    CHECK(!insv::ReadInsvThumbnail(bad).has_value());
    std::filesystem::remove(bad);
    const std::string other = writeTrailerFile("insv_test_thumb_other.insv", 0x400, rec);
    CHECK(!insv::ReadInsvThumbnail(other).has_value());
    std::filesystem::remove(other);
    CHECK(!insv::ReadInsvThumbnail("/nonexistent/clip.insv").has_value());
}

// --- a whole X5-shaped trailer, built record by record ---------------------

// The extended (MEI) calibration string's shape: "2_" then >= 55 numbers.
std::string extendedCalibrationString()
{
    std::string s = "2";
    for (int i = 0; i < 60; ++i) s += "_" + std::to_string(i % 7) + "." + std::to_string(i);
    return s;
}

// A file the way the X5 writes one: MP4 boxes (`ftyp`, `moov`), then the
// `inst` trailer to EOF with its offsets table (testtrailer::insvTrailer).
std::string writeX5Trailer(const char* name, const std::vector<TrailerRecord>& records)
{
    auto box = [](const char* type, size_t payloadBytes) {
        Bytes h;
        const uint32_t size = uint32_t(8 + payloadBytes);
        h.u8(uint8_t(size >> 24)).u8(uint8_t(size >> 16)).u8(uint8_t(size >> 8)).u8(uint8_t(size));
        h.str(type).raw(std::vector<uint8_t>(payloadBytes, 0x22));
        return h.b;
    };
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream f(path, std::ios::binary);
    for (const auto& part : {box("ftyp", 16), box("moov", 40), insvTrailer(records, true)})
        f.write(reinterpret_cast<const char*>(part.data()), std::streamsize(part.size()));
    return path;
}

// Record 0x101: the maker-notes strings first (tag, 1-byte length -- what
// ParseMakerNotes reads), then the protobuf fields timing and IMU scaling use,
// the colour profile (22; the raw-gyro one is filmed in I-Log, the other has
// none), the extended calibration (field 111) and the lens-profile table (145).
std::vector<uint8_t> metadataRecord(bool rawGyro)
{
    Bytes m;
    m.pbString(1, "IAHEA2500000001").pbString(2, "Insta360 X5").pbString(3, "v1.5.6_build1");
    m.pbString(5, "2_1.0_2.0_3.0");
    if (rawGyro) {
        m.pbString(22, "I_Log");
        Bytes crop; // window_crop_info: src 5376², dst 5312²
        crop.pbVarint(1, 5376).pbVarint(2, 5376).pbVarint(3, 5312).pbVarint(4, 5312);
        m.pbBytes(27, crop.b);
    }
    m.pbVarint(24, 2468081984ull);
    m.pbDouble(25, 21.244);
    m.pbDouble(28, 1.6);
    if (rawGyro) {
        m.pbVarint(62, 1);
        Bytes cfg;
        cfg.pbVarint(1, 32).pbVarint(2, 2000);
        m.pbBytes(65, cfg.b);
    }
    m.pbString(111, extendedCalibrationString());
    Bytes table;
    {
        Bytes poly;
        poly.pbString(1, "bare");
        for (double c : {0.0, 0.05, -1e-5, 2e-8, 0.0, 0.0}) poly.pbDouble(2, c);
        table.pbBytes(1, poly.b);
        Bytes protector;
        protector.pbString(1, "ProtectorA");
        Bytes packed;
        for (double c : {0.0, 0.049, 0.0, 0.0, 0.0, 0.0}) packed.f64(c);
        protector.pbBytes(2, packed.b); // packed doubles
        table.pbBytes(1, protector.b);
        Bytes fov;
        fov.pbString(1, "bare").pbDouble(2, 200.0);
        table.pbBytes(2, fov.b);
        Bytes fov2;
        fov2.pbString(1, "ProtectorA").pbDouble(2, 195.0);
        table.pbBytes(2, fov2.b);
    }
    m.pbBytes(145, table.b);
    return m.b;
}

void testTrailerRecords()
{
    // 0x300, raw counts (field 62): u64 time + 6 x u16, value = raw - 0x8000;
    // +-32 g and +-2000 deg/s full scale (field 65).
    Bytes imu;
    for (int i = 0; i < 3; ++i) {
        imu.u64(1000000 + uint64_t(i) * 1000);
        imu.u16(0x8000 + 1024).u16(0x8000).u16(0x8000 - 1024); // accel: +1 g, 0, -1 g
        imu.u16(0x8000 + 16384).u16(0x8000).u16(0x8000);       // gyro x: 1000 deg/s
    }
    // 0x400: u64 time, f64 exposure seconds.
    Bytes exposure;
    exposure.u64(1000500).f64(1.0 / 500).u64(1033833).f64(1.0 / 1000);
    // 0x700: 53-byte records at 10 Hz, the fix new once a second; the first
    // second without a fix.
    Bytes gps;
    for (int i = 0; i < 30; ++i) {
        Bytes r;
        r.u64(1757003465ull + uint64_t(i / 10)).u16(0).u8(i < 10 ? 'V' : 'A');
        r.raw(std::vector<uint8_t>(53 - 11, 0));
        gps.raw(r.b);
    }
    // 0x8000 TimeMap: one trim, (video ms, source ms) pairs 2002 ms apart.
    Bytes timeMap;
    timeMap.u32(32).u32(1).u32(3 * 16).u32(3).raw(std::vector<uint8_t>(16, 0));
    timeMap.f64(100.0).f64(2002.0).f64(2102.0);
    for (double v : {0.0, 33.37, 66.73}) timeMap.f64(v).f64(v + 2002.0);
    // An unknown record, stepped over.
    const std::vector<uint8_t> unknown(37, 0x5a);

    const std::string path = writeX5Trailer(
        "insv_test_x5.insv", {{0x101, metadataRecord(true)},
                              {0x300, imu.b},
                              {0x400, exposure.b},
                              {0x1c00, unknown},
                              {0x700, gps.b},
                              {0x8000, timeMap.b}});

    const auto notes = insv::ReadInsvMakerNotes(path);
    CHECK(notes.has_value());
    if (notes) {
        CHECK(notes->serial_number == "IAHEA2500000001");
        CHECK(notes->model == "Insta360 X5");
        CHECK(notes->firmware == "v1.5.6_build1");
        CHECK(notes->parameters == "2 1.0 2.0 3.0"); // underscores read as spaces
        CHECK(notes->lens_profiles.size() == 2);
        CHECK(notes->crop_dst_w == 5312.0);
        const insv::LensProfile* bare = insv::FindLensProfile(*notes, "bare");
        CHECK(bare && near(bare->poly[1], 0.05, 1e-12) && near(bare->poly[3], 2e-8, 1e-20));
        CHECK(bare && bare->fov_deg == 200.0);
        const insv::LensProfile* protector = insv::FindLensProfile(*notes, "ProtectorA");
        CHECK(protector && near(protector->poly[1], 0.049, 1e-12) && protector->fov_deg == 195.0);
        CHECK(!insv::FindLensProfile(*notes, "InvisibleDiveAir"));
    }

    const auto t = insv::ReadInsvTelemetry(path);
    CHECK(t.has_value());
    if (t) {
        CHECK(t->first_frame_timestamp == 2468081984ull);
        CHECK(near(t->rolling_shutter_ms, 21.244, 1e-12));
        CHECK(near(t->gyro_timestamp_ms, 1.6, 1e-12));
        CHECK(t->gamma_mode == "I_Log");
        CHECK(t->imu.size() == 3);
        if (t->imu.size() == 3) {
            CHECK(t->imu[1].timestamp == 1001000);
            CHECK(near(t->imu[0].accel[0], 1.0, 1e-12) && near(t->imu[0].accel[2], -1.0, 1e-12));
            CHECK(near(t->imu[0].gyro[0], 1000.0 * std::numbers::pi / 180.0, 1e-9));
            CHECK(t->imu[0].gyro[1] == 0.0);
        }
        CHECK(t->exposure.size() == 2);
        if (t->exposure.size() == 2)
            CHECK(t->exposure[1].timestamp == 1033833 && t->exposure[1].exposure_time == 1.0 / 1000);
        CHECK(t->gps_records == 30 && t->gps_fixes == 20);
        CHECK(near(t->gps_rate_hz, 1.0, 1e-9) && near(t->gps_track_s, 3.0, 1e-9));
        CHECK(near(t->time_map_offset_ms, 2002.0, 1e-9));
    }
    // A capped read stops early.
    const auto capped = insv::ReadInsvTelemetry(path, 2);
    CHECK(capped && capped->imu.size() == 2);

    const auto ext = insv::ReadInsvExtendedCalibrationString(path);
    CHECK(ext && *ext == extendedCalibrationString());

    // Everything after `moov`: the trailer, footer and all.
    const std::vector<uint8_t> raw = insv::ExtractRawTrailer(path);
    CHECK(raw.size() == std::filesystem::file_size(path) - 24 - 48);
    CHECK(raw.size() >= 32 && std::memcmp(raw.data() + raw.size() - 32, "8db42d694ccc418790edff439fe026bf", 32) == 0);
    std::filesystem::remove(path);

    // Without field 62 the IMU layout is guessed from the record length: 56
    // bytes of doubles, already in g and rad/s.
    Bytes doubles;
    for (int i = 0; i < 2; ++i) {
        doubles.u64(5000 + uint64_t(i));
        for (double v : {0.0, 0.0, 1.0, 0.1, 0.2, 0.3}) doubles.f64(v);
    }
    const std::string d = writeX5Trailer("insv_test_x5_doubles.insv",
                                         {{0x101, metadataRecord(false)}, {0x300, doubles.b}});
    const auto td = insv::ReadInsvTelemetry(d);
    CHECK(td && td->imu.size() == 2);
    if (td && td->imu.size() == 2)
        CHECK(td->imu[1].timestamp == 5001 && td->imu[1].accel[2] == 1.0 && td->imu[1].gyro[2] == 0.3);
    CHECK(td && td->exposure.empty() && td->gps_records == 0 && td->time_map_offset_ms == 0.0);
    CHECK(td && td->gamma_mode.empty()); // no field 22
    const auto nd = insv::ReadInsvMakerNotes(d);
    CHECK(nd && nd->crop_dst_w == 0.0); // no field 27
    std::filesystem::remove(d);

    // No trailer (no magic), no moov: nothing, and no crash.
    const std::string plain = (std::filesystem::temp_directory_path() / "insv_test_plain.mp4").string();
    {
        std::ofstream f(plain, std::ios::binary);
        const std::vector<char> junk(200, 0x33);
        f.write(junk.data(), std::streamsize(junk.size()));
    }
    CHECK(!insv::ReadInsvMakerNotes(plain).has_value());
    CHECK(!insv::ReadInsvTelemetry(plain).has_value());
    CHECK(!insv::ReadInsvExtendedCalibrationString(plain).has_value());
    CHECK(insv::ExtractRawTrailer(plain).empty());
    std::filesystem::remove(plain);
    CHECK(!insv::ReadInsvTelemetry("/nonexistent/clip.insv").has_value());
}

// A ONE X2-shaped trailer: a plain record chain (no offsets table), the
// 40-token offset_v3 in field 54 and no 56-token string, and telemetry
// stamped in milliseconds, which ReadInsvTelemetry hands out in microseconds
// at the model's tick (docs/formats/insta360-one-x2.md §4, §7, §8).
void testOneX2Trailer()
{
    std::printf("  ONE X2 trailer\n");
    Bytes meta;
    meta.pbString(1, "IXSE0000000001").pbString(2, "Insta360 ONE X2").pbString(3, "v1.0.92_build1");
    meta.pbString(22, "linear");
    meta.pbVarint(24, 4407);
    meta.pbDouble(25, 36.94);
    Bytes crop; // window_crop_info: src 3040², dst 2880²
    crop.pbVarint(1, 3040).pbVarint(2, 3040).pbVarint(3, 2880).pbVarint(4, 2880);
    meta.pbBytes(27, crop.b);
    meta.pbDouble(28, -12.0);
    meta.pbString(54, kOneX2V3);
    Bytes imu; // 56-byte doubles, 2 ms apart
    for (int i = 0; i < 3; ++i) {
        imu.u64(2800 + uint64_t(i) * 2);
        for (double v : {0.0, 0.0, 1.0, 0.1, 0.2, 0.3}) imu.f64(v);
    }
    Bytes exposure; // one per frame, 40 ms apart
    exposure.u64(4412).f64(0.001).u64(4452).f64(0.001);

    const std::string path = (std::filesystem::temp_directory_path() / "insv_test_onex2.insv").string();
    {
        std::ofstream f(path, std::ios::binary);
        const std::vector<uint8_t> video(64, 0x11); // stands in for the MP4 boxes
        const std::vector<uint8_t> t =
            insvTrailer({{0xa00, std::vector<uint8_t>(35, 0)}, {0x300, imu.b}, {0x400, exposure.b},
                         {0x101, meta.b}},
                        false);
        f.write(reinterpret_cast<const char*>(video.data()), std::streamsize(video.size()));
        f.write(reinterpret_cast<const char*>(t.data()), std::streamsize(t.size()));
    }

    const auto notes = insv::ReadInsvMakerNotes(path);
    CHECK(notes && notes->model == "Insta360 ONE X2" && notes->crop_dst_w == 2880.0);
    CHECK(notes && notes->offset_v3 == kOneX2V3);
    CHECK(notes && notes->lens_profiles.empty());
    CHECK(!insv::ReadInsvExtendedCalibrationString(path).has_value());

    bool known = false;
    const insv::InsvModel& model = insv::FindInsvModel("Insta360 ONE X2", &known);
    CHECK(known && model.telemetryTickUs == 1000.0);
    CHECK(model.secondLensToken && std::string(model.secondLensToken) == "_10_");
    CHECK(std::string(model.captureMode(2880)) == "5.7K");

    const auto raw = insv::ReadInsvTelemetry(path); // the file's own ticks
    CHECK(raw && raw->imu.size() == 3 && raw->imu[1].timestamp == 2802);
    const auto t = insv::ReadInsvTelemetry(path, 0, model.telemetryTickUs);
    CHECK(t.has_value());
    if (t) {
        CHECK(t->imu.size() == 3 && t->imu[1].timestamp == 2802000);
        CHECK(t->imu.size() == 3 && t->imu[2].accel[2] == 1.0 && t->imu[2].gyro[1] == 0.2);
        CHECK(t->exposure.size() == 2 && t->exposure[1].timestamp == 4452000);
        CHECK(t->first_frame_timestamp == 4407000);
        CHECK(near(t->gyro_timestamp_ms, -12.0, 1e-12)); // already ms, unscaled
        CHECK(near(t->rolling_shutter_ms, 36.94, 1e-12));
        CHECK(t->gamma_mode == "linear");
    }
    std::filesystem::remove(path);
}

// The Y'CbCr -> R'G'B' rows the viewport converts lens planes with must match
// the stitch's Vulkan ycbcr conversion for every matrix, range and depth.
void testYcbcrRows()
{
    using insv::YcbcrMatrix;
    // BT.709 full range, 8-bit: the X5's, and the constants the viewport used
    // before the conversion came from the stream.
    float r[3][4];
    insv::YcbcrToRgbRows({YcbcrMatrix::Bt709, true}, 8, r);
    CHECK(near(r[0][0], 1.0, 1e-6) && near(r[0][1], 0.0, 1e-6) && near(r[0][2], 1.5748, 1e-4));
    CHECK(near(r[1][1], -0.1873, 1e-4) && near(r[1][2], -0.4681, 1e-4));
    CHECK(near(r[2][1], 1.8556, 1e-4) && near(r[2][2], 0.0, 1e-6));
    CHECK(near(r[0][3], -1.5748 * 128.0 / 255.0, 1e-4));

    struct Weights { YcbcrMatrix m; double kr, kb; };
    for (const Weights w : {Weights{YcbcrMatrix::Bt601, 0.299, 0.114},
                            Weights{YcbcrMatrix::Bt709, 0.2126, 0.0722},
                            Weights{YcbcrMatrix::Bt2020, 0.2627, 0.0593}}) {
        for (const bool full : {false, true}) {
            for (const int depth : {8, 10}) {
                insv::YcbcrToRgbRows({w.m, full}, depth, r);
                const double step = depth == 8 ? 1.0 : 4.0;
                const double maxCode = depth == 8 ? 255.0 : 1023.0;
                // A code value as the plane's texel reads it (10-bit: P010).
                const auto texel = [&](double code) {
                    return depth == 8 ? code / 255.0 : code * 64.0 / 65535.0;
                };
                // Encode R'G'B' as the stream would, then convert back.
                const auto roundTrip = [&](double R, double G, double B, double out[3]) {
                    const double y = w.kr * R + (1 - w.kr - w.kb) * G + w.kb * B;
                    const double cb = (B - y) / (2 * (1 - w.kb));
                    const double cr = (R - y) / (2 * (1 - w.kr));
                    const double yc = full ? y * maxCode : 16 * step + 219 * step * y;
                    const double cs = full ? maxCode : 224 * step;
                    const double t[3] = {texel(yc), texel(128 * step + cs * cb),
                                         texel(128 * step + cs * cr)};
                    for (int i = 0; i < 3; ++i)
                        out[i] = r[i][0] * t[0] + r[i][1] * t[1] + r[i][2] * t[2] + r[i][3];
                };
                const double colours[][3] = {
                    {0, 0, 0}, {1, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.2, 0.5, 0.8}};
                for (const auto& c : colours) {
                    double out[3];
                    roundTrip(c[0], c[1], c[2], out);
                    for (int i = 0; i < 3; ++i)
                        CHECK(near(out[i], c[i], 1e-5));
                }
            }
        }
    }
}

int main()
{
    testExtendedCalibration();
    testCalibrationV3();
    testMeiProjection();
    testLensProfile();
    testLinearAlgebra();
    testSyntheticRender();
    testSeamDepthGeometry();
    testSeamDepthTracker();
    testSeamAlignment();
    testBlurQuads();
    testTrailerThumbnail();
    testTrailerRecords();
    testOneX2Trailer();
    testYcbcrRows();

    if (g_failures == 0)
        std::printf("insv_test: all checks passed\n");
    else
        std::printf("insv_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
