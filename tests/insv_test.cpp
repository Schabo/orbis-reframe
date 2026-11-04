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
#include "Gps.h"
#include "Image.h"
#include "InsvModels.h"
#include "InsvTrailer.h"
#include "Motion.h"
#include "SeamAlignment.h"
#include "SeamColourBlend.h"
#include "SeamCut.h"
#include "SeamDepth.h"
#include "SeamFlow.h"
#include "SeamWindow.h"
#include "Smooth.h"
#include "Ycbcr.h"
#include "support/CommaLocale.h"
#include "support/TestTrailer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
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

    // The file's '.' decimals parse the same under a decimal-comma locale.
    const testlocale::CommaLocale comma;
    if (comma.name().empty()) {
        std::printf("  (no decimal-comma locale installed; skipping the locale check)\n");
        return;
    }
    insv::InsvCalibration c4;
    CHECK(insv::ParseExtendedCalibrationString(kX5Extended, c4));
    CHECK(c4.lensesExt.size() == 2 && near(c4.lensesExt[0].fx, 4294.030, 1e-3));
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

// The extrinsics compose pitch, then yaw, then roll. With the Antigravity
// A1's pitch -90, yaw then turns about the lens's own axis, so it doesn't
// tilt that axis: the two lens axes stay opposite to within the roll
// residuals (0.4 and 0.56 deg here, 0.17 deg apart). Composed yaw last,
// lens 0's 2.5 deg yaw deviation tilted its axis 2.9 deg off lens 1's.
void testExtrinsicsOrder()
{
    std::printf("  extrinsics: pitch, yaw, roll\n");
    insv::InsvCalibration c;
    CHECK(insv::ParseCalibrationV3String(
        "2_1.5_3742.5_3741.8_2620.9_2617.6_-177.49_-89.95_90.397_0_0_0_"
        "-0.056_0.204_-0.534_0.0008_0.00004_10496_5248_155_"
        "1.5_3754.8_3755.1_7875.9_2606.2_-179.449_-89.922_89.441_-0.0001_0.00002_"
        "-0.071_-0.050_0.170_-0.499_0.0001_0.00003_10496_5248_155_197632",
        c));
    for (auto& e : c.lensesExt)
        e.focalRefW = 5248.0;
    insv::FisheyeStitcher st;
    CHECK(st.configure(c, 3840, 3840, insv::StitchParams{}));
    if (!st.ready())
        return;
    // Each lens's optical axis in the world is its worldToLens's third row.
    const double* a0 = st.lensProjection(0).worldToLens.m[2];
    const double* a1 = st.lensProjection(1).worldToLens.m[2];
    const double dot = a0[0] * a1[0] + a0[1] * a1[1] + a0[2] * a1[2];
    const double offOppositeDeg = std::acos(std::clamp(-dot, -1.0, 1.0)) * 180.0 / M_PI;
    CHECK(offOppositeDeg < 0.3);
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

// SeamPatchScore matches texture, not shading: the same texture under two
// different brightness planes is a match, a shared gradient over two
// different textures (glare from a low sun on the seam) is not.
void testSeamPatchScore()
{
    constexpr int N = insv::kSeamPatchAz * insv::kSeamPatchEl;
    uint32_t seed = 12345;
    auto noise = [&] { // -1..1
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / (1u << 23) - 1.0;
    };
    double t0[N], t1[N];
    for (int s = 0; s < N; ++s)
        t0[s] = noise(), t1[s] = noise();
    auto score = [&](auto&& x, auto&& y, bool (*keep)(int), float& ncc, float& tex) {
        double sums[insv::kSeamPatchSums] = {};
        for (int s = 0; s < N; ++s)
            if (keep(s)) {
                const double a = s % insv::kSeamPatchAz - 7.5, e = s / insv::kSeamPatchAz - 7.5;
                insv::SeamPatchAccumulate(sums, s, x(s, a, e), y(s, a, e));
            }
        insv::SeamPatchScore(sums, ncc, tex);
    };
    auto all = [](int) { return true; };
    // A corner of the patch outside the lens: the plane still comes out exactly.
    auto corner = [](int s) { return !(s % insv::kSeamPatchAz < 6 && s / insv::kSeamPatchAz < 6); };
    float ncc, tex;
    for (auto keep : {+all, +corner}) {
        score([&](int s, double a, double e) { return 100 + 10 * t0[s] + 3 * a + 1 * e; },
              [&](int s, double a, double e) { return 140 + 10 * t0[s] - 2 * a - 6 * e; }, keep, ncc,
              tex);
        CHECK(ncc > 0.999f);
        CHECK(near(tex, 10 * std::sqrt(1.0 / 3), 0.6)); // the texture's std, not the gradient's
        score([&](int s, double, double e) { return 128 + 10 * e + 4 * t0[s]; },
              [&](int s, double, double e) { return 128 + 10 * e + 4 * t1[s]; }, keep, ncc, tex);
        CHECK(std::fabs(ncc) < 0.2f);
    }
    // Flat but for the plane: no NCC; under a quarter of the samples: none at all.
    score([](int, double a, double) { return 50 + 4 * a; }, [](int, double, double e) { return 60 + 2 * e; },
          +all, ncc, tex);
    CHECK(ncc == 0.0f && tex < 0.01f);
    score([&](int s, double, double) { return 100 + 10 * t0[s]; },
          [&](int s, double, double) { return 100 + 10 * t0[s]; }, +[](int s) { return s < N / 5; }, ncc,
          tex);
    CHECK(ncc < -1.5f);
}

// The seam window's depth table on a synthetic cost volume: a near object
// over one arc of bins, far elsewhere, and textureless bins that must follow
// their neighbours.
void testSeamDepthWindow()
{
    constexpr int B = insv::kSeamDepthBins, K = insv::kSeamDepthCands;
    const double band = 5.0 * kDeg;
    auto costs = std::make_unique<insv::SeamCosts>();
    auto stats = std::make_unique<insv::SeamCutStats>(); // no cells: the cut keeps out of it
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
    insv::SeamWindow w;
    w.configure(0.032, band);
    insv::SeamState s;
    CHECK(!w.solve(1000, s));
    fill(1.8);
    w.add(1000, *costs, *stats);
    CHECK(w.solve(1000, s));
    const float* q = s.q;
    CHECK(near(q[50], 1.8, 0.1) && near(q[60], 1.8, 0.1));
    CHECK(near(q[55], 1.8, 0.15));                    // textureless: from its neighbours
    CHECK(near(q[10], 0.0, 0.1) && near(q[100], 0.0, 0.1) && near(q[120], 0.0, 0.1));
    CHECK(q[40 - 3] < 0.3 && q[70 + 2] < 0.3);         // the edge stays an edge

    // A rival depth that matches clearly better for one frame (the road
    // behind a crash bar) doesn't take the bins over, even at that frame:
    // the frames either side outvote it. With a one-frame window it does.
    auto rival = [&](double nearQ, double farQ, int from, int to) {
        fill(nearQ);
        for (int b = from; b < to; ++b)
            for (int k = 0; k < K; ++k) {
                const double qq = insv::SeamCandQ(k);
                const double dn = std::fabs(qq - nearQ), df = std::fabs(qq - farQ);
                costs->ncc[b][k] = static_cast<float>(
                    std::max(0.70 - 0.8 * std::min(dn, 1.0), 0.95 - 0.8 * std::min(df, 1.0)));
            }
    };
    insv::SeamWindow single;
    single.tuning.depthHalfMs = 0;
    single.configure(0.032, band);
    w.reset();
    for (int f = 0; f < 15; ++f) {
        if (f == 7)
            rival(1.8, 0.3, 55, 62);
        else
            fill(1.8);
        w.add(1000 + 33 * f, *costs, *stats);
        single.add(1000 + 33 * f, *costs, *stats);
    }
    insv::SeamState s1;
    w.solve(1000 + 33 * 7, s);
    single.solve(1000 + 33 * 7, s1);
    CHECK(s.q[58] > 1.6);
    CHECK(s1.q[58] < 1.0);

    // The object moves closer and stays there: the table follows within the
    // window, and doesn't move before the window reaches the change.
    for (int f = 15; f < 30; ++f) {
        fill(2.4);
        w.add(1000 + 33 * f, *costs, *stats);
    }
    w.solve(1000 + 33 * 20, s);
    CHECK(near(s.q[60], 2.4, 0.1));
    w.solve(1000 + 33 * 10, s);
    CHECK(near(s.q[60], 1.8, 0.1));
    // A frame not after the latest starts over.
    fill(1.2);
    w.add(500, *costs, *stats);
    w.solve(500, s);
    CHECK(near(s.q[60], 1.2, 0.1));
    w.reset();
    CHECK(w.empty() && !w.solve(500, s));
}

// The depth confidence on matches that can't tell the distance: one clean
// peak counts fully; a smooth ramp, a repeated texture and scattered peaks
// of the same height don't. And the table stays within the shift range the
// camera's baseline gives (the X5's shift: the A1 stops at ~0.7 m).
void testSeamDepthUniqueness()
{
    constexpr int B = insv::kSeamDepthBins, K = insv::kSeamDepthCands;
    auto costs = std::make_unique<insv::SeamCosts>();
    auto data = std::make_unique<insv::SeamDepthFrame>();
    const insv::SeamDepthTuning tuning;
    auto confidence = [&](const std::function<double(int)>& ncc) {
        for (int b = 0; b < B; ++b)
            for (int k = 0; k < K; ++k) {
                costs->ncc[b][k] = static_cast<float>(ncc(k));
                costs->texture[b][k] = 30.0f;
            }
        insv::SeamDepthData(*costs, 0.032, tuning, *data); // the X5's 32 mm
        return data->meanConf;
    };
    const double clean = confidence([](int k) {
        const double dq = insv::SeamCandQ(k) - 1.0;
        return 0.9 * std::exp(-dq * dq / (2 * 0.15 * 0.15));
    });
    const double ramp = confidence([](int k) { return 0.2 + 0.7 * k / (K - 1.0); });
    const double periodic = confidence([](int k) { return 0.45 + 0.45 * std::cos(2 * std::numbers::pi * k / 8.0); });
    const double scattered = confidence([](int k) {
        static const double v[] = {0.3, 0.5, 0.82, 0.4, 0.35, 0.6, 0.85, 0.3, 0.45, 0.7};
        return v[k % 10];
    });
    CHECK(clean > 0.95);
    CHECK(ramp < 0.05);
    CHECK(periodic < 0.05);
    CHECK(scattered < 0.2);

    // A1 (71 mm): the best match at 2.5 1/m (a 10 deg shift) is out of
    // range; the table takes the weaker one at 1.0 1/m.
    for (int b = 0; b < B; ++b)
        for (int k = 0; k < K; ++k) {
            const double q = insv::SeamCandQ(k);
            costs->ncc[b][k] = static_cast<float>(std::max(0.95 - 2.0 * std::fabs(q - 2.5),
                                                           0.8 - 2.0 * std::fabs(q - 1.0)));
            costs->texture[b][k] = 30.0f;
        }
    float x5[B], a1[B];
    insv::SeamDepthData(*costs, 0.032, tuning, *data);
    insv::SolveSeamDepth(&data->data[0][0], 0.032, tuning, x5);
    insv::SeamDepthData(*costs, 0.071, tuning, *data);
    insv::SolveSeamDepth(&data->data[0][0], 0.071, tuning, a1);
    CHECK(near(x5[20], 2.5, 0.15));
    CHECK(near(a1[20], 1.0, 0.15));
    for (int b = 0; b < B; ++b)
        CHECK(0.071 * a1[b] <= insv::kSeamShiftMaxRad + 1e-6);
}

// The state at a frame is the same whichever way it is reached: played into
// from the start of the clip, or from lookBackMs() before it (a seek, an
// export's own pass), each frame's cut statistics measured on the window's
// measureTable(). The scene changes from frame to frame and the statistics
// depend on the table they were measured on, so a shorter run-up would
// differ.
void testSeamWindowPathIndependence()
{
    constexpr int B = insv::kSeamDepthBins, K = insv::kSeamDepthCands, R = insv::kSeamCutRows;
    const double band = 5.0 * kDeg;
    auto costs = std::make_unique<insv::SeamCosts>();
    auto stats = std::make_unique<insv::SeamCutStats>();
    insv::LensColour clip;
    clip.logRatio[0] = 0.05;
    auto frame = [&](int f, const float* qMeas) {
        const double nearQ = 1.0 + 0.03 * f;
        const int from = 30 + f % 7;
        for (int b = 0; b < B; ++b) {
            const double truth = b >= from && b < from + 25 ? nearQ : 0.0;
            for (int k = 0; k < K; ++k) {
                const double dq = insv::SeamCandQ(k) - truth;
                costs->ncc[b][k] = static_cast<float>(0.95 - 0.8 * std::min(std::fabs(dq), 1.0) -
                                                      0.02 * ((b * 7 + f * 3) % 5));
                costs->texture[b][k] = 30.0f;
            }
            for (int r = 0; r < R; ++r) {
                insv::SeamCutStats::Cell& c = stats->cell[b][r];
                c.n = 64;
                c.texture = 20;
                // Agreement where the table measured on is right.
                c.zsad = static_cast<float>(r == 4 ? 2.0 + 10.0 * std::fabs(qMeas[b] - truth) : 12.0);
                for (int k = 0; k < 3; ++k) {
                    c.nColour[k] = 64;
                    c.mean[1][k] = 100;
                    c.mean[0][k] = static_cast<float>(100 + (k == 0 ? 6 + f % 5 : 0) + (b > 90 ? 8 : 0));
                }
            }
        }
    };
    auto feed = [&](insv::SeamWindow& w, int from, int to) {
        for (int f = from; f <= to; ++f) {
            float q[B];
            w.measureTable(1000 + 33 * f, q);
            frame(f, q);
            w.add(1000 + 33 * f, *costs, *stats);
        }
    };
    insv::SeamWindow played, seeked, short_;
    for (insv::SeamWindow* w : {&played, &seeked, &short_}) {
        w->configure(0.032, band);
        w->setLensColour(clip);
    }
    const int n = 60, at = 40;
    feed(played, 0, n);
    const int64_t t = 1000 + 33 * at;
    int first = 0;
    while (1000 + 33 * first < t - played.lookBackMs())
        ++first;
    int last = at;
    while (1000 + 33 * (last + 1) <= t + played.lookAheadMs())
        ++last;
    feed(seeked, first, last);
    feed(short_, first + 6, last);
    insv::SeamState a, b, c;
    CHECK(played.solve(t, a) && seeked.solve(t, b) && short_.solve(t, c));
    CHECK(std::memcmp(a.q, b.q, sizeof a.q) == 0);
    CHECK(std::memcmp(&a.blend, &b.blend, sizeof a.blend) == 0);
    CHECK(std::memcmp(&a.blend, &c.blend, sizeof a.blend) != 0); // the run-up matters

    // Half precision round trip.
    for (float v : {0.0f, 1.0f, -2.5f, 0.0123f, 3.2f, 1e-6f, 65504.0f})
        CHECK(near(insv::HalfToFloat(insv::FloatToHalf(v)), v, std::fabs(v) / 1024.0 + 1e-7));
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

// The seam cut's blend: weights from the per-bin cut and fade, the plain
// cross-fade without one.
void testSeamBlend()
{
    insv::InsvCalibration c = x5Calibration();
    insv::StitchParams p;
    p.depthSeam = true;
    insv::FisheyeStitcher st;
    CHECK(st.configure(c, 3840, 3840, p));
    const double band = st.gpuParams().bandRad;
    const double phi = 1.234;

    // No blend, or one not measured yet (fade 0): today's cross-fade.
    insv::SeamBlend none;
    for (double e : {-6.0, -2.0, 0.0, 1.0, 4.5}) {
        const insv::Vec3 d = st.seamDir(phi, e * kDeg);
        double w[2], wz[2];
        st.seamBlendWeights(d, nullptr, w);
        st.seamBlendWeights(d, &none, wz);
        for (int i = 0; i < 2; ++i)
            CHECK(w[i] == st.seamBlendWeight(i, d) && wz[i] == w[i]);
    }

    // A narrow fade around a cut 1 deg towards lens 0: lens 0 alone above
    // it, lens 1 alone below, half and half on it, adding up to 1.
    insv::SeamBlend narrow;
    for (int b = 0; b < insv::kSeamDepthBins; ++b) {
        narrow.cut[b] = static_cast<float>(1.0 * kDeg);
        narrow.fade[b] = static_cast<float>(0.75 * kDeg);
    }
    double w[2];
    st.seamBlendWeights(st.seamDir(phi, 1.0 * kDeg), &narrow, w);
    CHECK(near(w[0], 0.5, 1e-4) && near(w[0] + w[1], 1.0, 1e-12));
    st.seamBlendWeights(st.seamDir(phi, 1.8 * kDeg), &narrow, w);
    CHECK(w[0] == 1.0 && w[1] == 0.0);
    st.seamBlendWeights(st.seamDir(phi, 0.2 * kDeg), &narrow, w);
    CHECK(w[0] == 0.0 && w[1] == 1.0);
    st.seamBlendWeights(st.seamDir(phi, 1.4 * kDeg), &narrow, w);
    CHECK(w[0] > 0.8 && w[0] < 1.0);

    // The full band around the seam circle is today's cross-fade (up to the
    // lens axes being a little off the seam frame's).
    insv::SeamBlend wide;
    for (int b = 0; b < insv::kSeamDepthBins; ++b)
        wide.fade[b] = static_cast<float>(band);
    for (double e : {-4.0, -1.0, 0.0, 2.0, 4.0}) {
        const insv::Vec3 d = st.seamDir(phi, e * kDeg);
        st.seamBlendWeights(d, &wide, w);
        CHECK(near(w[0], st.seamBlendWeight(0, d), 0.02) && near(w[1], st.seamBlendWeight(1, d), 0.02));
    }

    // The lens colour: lens 0 20 % brighter in red, 10 % darker in blue, and
    // green rising 1 % per degree towards lens 0. Both lenses go to the
    // geometric mean, the slope held at the band's edge beyond it, the
    // frame's residual added.
    insv::StitchParams pc = p;
    pc.lensColour.logRatio[0] = std::log(1.2);
    pc.lensColour.logRatio[2] = std::log(0.9);
    pc.lensColour.slope[1] = 0.01;
    insv::FisheyeStitcher sc;
    CHECK(sc.configure(c, 3840, 3840, pc));
    double lg[2][3], vl[2][3];
    sc.lensLogGains(sc.seamDir(phi, 2.0 * kDeg), nullptr, lg, vl);
    CHECK(near(lg[1][0] - lg[0][0], std::log(1.2), 1e-6) && near(lg[0][0] + lg[1][0], 0.0, 1e-12));
    CHECK(near(lg[1][1] - lg[0][1], 0.02, 1e-6));
    CHECK(near(lg[1][2] - lg[0][2], std::log(0.9), 1e-6));
    const double edgeG = 0.01 * band / kDeg;
    sc.lensLogGains(sc.seamDir(phi, band + 20.0 * kDeg), nullptr, lg, vl);
    CHECK(near(lg[1][1] - lg[0][1], edgeG, 1e-6) && near(lg[1][0] - lg[0][0], std::log(1.2), 1e-6));
    sc.lensLogGains(sc.seamDir(phi, -60.0 * kDeg), nullptr, lg, vl);
    CHECK(near(lg[1][1] - lg[0][1], -edgeG, 1e-6));
    insv::SeamBlend residual;
    residual.colour[0] = 0.15f;
    residual.colour[2] = -0.05f;
    sc.lensLogGains(sc.seamDir(phi, 0.0), &residual, lg, vl);
    CHECK(near(lg[1][0] - lg[0][0], insv::kLensColourMaxLog, 1e-6)); // bounded
    CHECK(near(lg[1][2] - lg[0][2], std::log(0.9) - 0.05, 1e-6));
    // The GPU gets the same, per radian.
    CHECK(near(sc.gpuParams().lensColourSlope[1], 0.01 / kDeg, 1e-6));
    // The local correction: whole in the band, half way along the ramp past
    // it, gone beyond; the sum bounded.
    insv::SeamBlend local;
    for (int b = 0; b < insv::kSeamDepthBins; ++b)
        local.local[b][1] = 0.1f;
    sc.lensLogGains(sc.seamDir(phi, 1.0 * kDeg), &local, lg, vl);
    CHECK(near(lg[1][1] - lg[0][1], 0.01 + 0.1, 1e-6));
    sc.lensLogGains(sc.seamDir(phi, band + 0.5 * insv::kLensColourLocalRampDeg * kDeg), &local, lg, vl);
    CHECK(near(lg[1][1] - lg[0][1], edgeG + 0.05, 1e-6));
    sc.lensLogGains(sc.seamDir(phi, band + insv::kLensColourLocalRampDeg * kDeg + 0.01), &local, lg, vl);
    CHECK(near(lg[1][1] - lg[0][1], edgeG, 1e-6));
    local.local[3][0] = local.local[4][0] = 0.25f;
    sc.lensLogGains(sc.seamDir((3.5 + 0.5) * 2.0 * std::numbers::pi / insv::kSeamDepthBins, 0.0),
                    &local, lg, vl);
    CHECK(near(lg[1][0] - lg[0][0], insv::kLensColourMaxLog, 1e-6));
    CHECK(vl[0][1] == 0.0 && vl[1][1] == 0.0);
    // The veiling light likewise, to the lens that has it.
    insv::SeamBlend veiled;
    for (int b = 0; b < insv::kSeamDepthBins; ++b)
        veiled.veil[b][1] = -0.1f; // lens 1
    sc.lensLogGains(sc.seamDir(phi, 1.0 * kDeg), &veiled, lg, vl);
    CHECK(vl[0][1] == 0.0 && near(vl[1][1], 0.1, 1e-6) && vl[1][0] == 0.0);
    sc.lensLogGains(sc.seamDir(phi, band + 0.5 * insv::kLensColourLocalRampDeg * kDeg), &veiled, lg, vl);
    CHECK(near(vl[1][1], 0.05, 1e-6));
    sc.lensLogGains(sc.seamDir(phi, -band - insv::kLensColourLocalRampDeg * kDeg - 0.01), &veiled, lg, vl);
    CHECK(vl[1][1] == 0.0);
    // Taken out in linear light: a dark level loses relatively more than a
    // bright one, never below kLensVeilKeep of its light; nothing over the
    // knee; still rising with the level.
    const auto lin = [](double v) { return std::pow(v / 255.0, insv::kLensVeilGamma); };
    CHECK(insv::LensVeilRemove(100.0, 0.0) == 100.0);
    CHECK(near(lin(insv::LensVeilRemove(150.0, 0.1)), lin(150.0) - 0.1, 1e-9));
    CHECK(near(lin(insv::LensVeilRemove(40.0, 0.1)), insv::kLensVeilKeep * lin(40.0), 1e-9));
    CHECK(insv::LensVeilRemove(insv::kLensColourKneeHi, 0.2) == insv::kLensColourKneeHi);
    double lastV = -1.0;
    for (double v = 0.5; v <= 255.0; v += 0.5) {
        const double out = insv::LensVeilRemove(v, 0.15);
        CHECK(out > lastV);
        lastV = out;
    }

    // Highlights: the gain eases to 1 over the knee, a channel still rising.
    CHECK(near(insv::LensColourGain(0.1, 100.0), std::exp(0.1), 1e-12));
    CHECK(insv::LensColourGain(0.1, insv::kLensColourKneeHi) == 1.0);
    CHECK(insv::LensColourGain(-0.1, 255.0) == 1.0);
    for (double lgv : {-0.5 * insv::kLensColourMaxLog, 0.5 * insv::kLensColourMaxLog}) {
        double last = 0.0;
        for (double v = 0.5; v <= 255.0; v += 0.5) {
            const double out = v * insv::LensColourGain(lgv, v);
            CHECK(out > last);
            last = out;
        }
    }

    // Off: the blend is ignored.
    insv::FisheyeStitcher off;
    CHECK(off.configure(c, 3840, 3840, insv::StitchParams{}));
    const insv::Vec3 d = off.seamDir(phi, 0.2 * kDeg);
    off.seamBlendWeights(d, &narrow, w);
    CHECK(w[0] == off.seamBlendWeight(0, d) && w[1] == off.seamBlendWeight(1, d));
}

namespace {

// Costs that match at infinity everywhere: the depth side quiet, for the
// cut and colour tests.
std::unique_ptr<insv::SeamCosts> farCosts()
{
    auto c = std::make_unique<insv::SeamCosts>();
    for (int b = 0; b < insv::kSeamDepthBins; ++b)
        for (int k = 0; k < insv::kSeamDepthCands; ++k) {
            c->ncc[b][k] = static_cast<float>(0.95 - 0.8 * std::min(std::fabs(insv::SeamCandQ(k)), 1.0));
            c->texture[b][k] = 30.0f;
        }
    return c;
}

// The state of a window holding `stats` alone, at band half-width `band`.
insv::SeamState solveOne(const insv::SeamCutStats& stats, double band,
                         const insv::LensColour& clip = {})
{
    insv::SeamWindow w;
    w.configure(0.032, band);
    w.setLensColour(clip);
    w.add(1000, *farCosts(), stats);
    insv::SeamState s;
    w.solve(1000, s);
    return s;
}

} // namespace

// The seam window's cut on synthetic statistics: the lenses agree best on
// one row everywhere but an arc where no row agrees (an occlusion).
void testSeamCutWindow()
{
    constexpr int B = insv::kSeamDepthBins, R = insv::kSeamCutRows;
    const double band = 5.0 * kDeg, narrow = insv::kSeamCutFadeDeg * kDeg;
    const double half = 1e-4; // the state's half precision
    auto stats = std::make_unique<insv::SeamCutStats>();
    auto costs = farCosts();
    auto fill = [&](int bestRow, float bestZsad) {
        for (int b = 0; b < B; ++b)
            for (int r = 0; r < R; ++r) {
                insv::SeamCutStats::Cell& cell = stats->cell[b][r];
                cell.n = 64;
                cell.texture = 20;
                cell.zsad = (b >= 80 && b < 90) ? 40.0f : (r == bestRow ? bestZsad : 12.0f);
            }
    };
    fill(6, 2.0f);
    insv::SeamState s = solveOne(*stats, band);
    const insv::SeamBlend& bl = s.blend;
    CHECK(near(bl.cut[20], insv::SeamCutRowE(6, band), half) && near(bl.fade[20], narrow, half));
    CHECK(near(bl.cut[84], 0.0, half) && near(bl.fade[84], band, half)); // occluded: the full fade
    CHECK(bl.fade[79] > narrow && bl.fade[79] < band);                  // widening gradually
    CHECK(near(bl.fade[76], narrow, half));
    for (int b = 0; b < B; ++b) // inside the band
        CHECK(std::fabs(bl.cut[b]) + bl.fade[b] <= band + half);

    // One frame of a slightly better row among its neighbours doesn't move
    // the cut there ...
    insv::SeamWindow w;
    w.configure(0.032, band);
    for (int f = 0; f < 9; ++f) {
        if (f == 4)
            fill(2, 1.5f);
        else
            fill(6, 2.0f);
        w.add(1000 + 33 * f, *costs, *stats);
    }
    w.solve(1000 + 33 * 4, s);
    CHECK(near(bl.cut[20], insv::SeamCutRowE(6, band), half));
    // ... a clearly better one, kept up, does.
    w.reset();
    fill(2, 0.5f);
    for (int f = 0; f < 9; ++f)
        w.add(1000 + 33 * f, *costs, *stats);
    w.solve(1000 + 33 * 4, s);
    CHECK(near(bl.cut[20], insv::SeamCutRowE(2, band), half));

    // Rows the lenses don't both see are never cut through.
    fill(6, 2.0f);
    for (int b = 0; b < B; ++b)
        stats->cell[b][6].n = 10;
    s = solveOne(*stats, band);
    for (int b = 0; b < 80; ++b)
        CHECK(!near(bl.cut[b], insv::SeamCutRowE(6, band), half));

    // The colour residual against the clip's lens colour: lens 0 (120, 100,
    // 80) against lens 1 (100, 100, 100) where the lenses agree; nothing
    // usable (clipped) over bins 20..39; and bins 60..63 far off (a
    // reflection each lens sees differently).
    fill(6, 2.0f);
    auto colours = [&](float red0) {
        for (int b = 0; b < B; ++b)
            for (int r = 0; r < R; ++r) {
                insv::SeamCutStats::Cell& cell = stats->cell[b][r];
                for (int k = 0; k < 3; ++k)
                    cell.nColour[k] = b >= 20 && b < 40 ? 0.0f : 64.0f;
                const float m0[3] = {red0, 100, 80};
                for (int k = 0; k < 3; ++k)
                    cell.mean[0][k] = m0[k], cell.mean[1][k] = 100;
                if (b >= 60 && b < 64)
                    cell.mean[0][1] = 160;
            }
    };
    colours(120);
    insv::LensColour clip;
    clip.logRatio[0] = std::log(1.15);
    clip.logRatio[2] = std::log(0.8);
    s = solveOne(*stats, band, clip);
    const float* g = bl.colour;
    const double r120 = std::log(1.2 / 1.15), r100 = -std::log(1.15);
    CHECK(near(g[0], r120, half) && near(g[1], 0.0, half) && near(g[2], 0.0, half));
    // Over the frames within half a second either side: one frame of equal
    // reds among 31 moves it a 31st of the way.
    w.reset();
    w.setLensColour(clip);
    auto run = [&](const std::function<void(int)>& frame) {
        w.reset();
        for (int f = 0; f <= 30; ++f) {
            frame(f);
            w.add(1000 + 33 * f, *costs, *stats);
        }
        w.solve(1000 + 33 * 15, s);
    };
    run([&](int f) { colours(f == 15 ? 100.0f : 120.0f); });
    CHECK(near(g[0], (30 * r120 + r100) / 31, 2 * half));
    // A frame with flare (veiling light on most of the seam, unevenly: a
    // wide spread) or with nothing usable is left out.
    run([&](int f) {
        colours(120);
        if (f == 15)
            for (int b = 0; b < 100; ++b)
                for (int r = 0; r < R; ++r)
                    stats->cell[b][r].mean[1][1] = 100.0f * (1.1f + 0.05f * (b % 10));
    });
    CHECK(near(g[0], r120, half));
    run([&](int f) {
        colours(120);
        if (f == 15)
            for (int b = 0; b < B; ++b)
                for (int r = 0; r < R; ++r)
                    for (float& n : stats->cell[b][r].nColour)
                        n = 0;
    });
    CHECK(near(g[0], r120, half));
    // Measured against a new lens colour at once; bounded.
    clip.logRatio[0] = std::log(1.2);
    w.setLensColour(clip);
    w.solve(1000 + 33 * 15, s);
    CHECK(near(g[0], 0.0, half));
    colours(150); // log(1.5 / 1.2) = 0.22: bounded
    s = solveOne(*stats, band, clip);
    CHECK(near(g[0], insv::kLensColourResidualMaxLog, half));
}

// The new stitcher's seam colour blend: the fade across the seam; a known
// per-bin colour ratio recovered from one frame's cells, with a sky clipped
// in blue and a flare patch on one lens; the window pooling a frame with
// the second before it, starting afresh after a seek; the CPU stitch moving each lens half of it near the
// seam and nothing far from it.
// The seam flow (SeamFlow.h): the share across the seam, the grid, the
// field's lookup, the window over frames, and the warp through the stitch.
void testSeamFlow()
{
    std::printf("  seam flow\n");
    const double reach = 8.0 * kDeg;
    CHECK(insv::SeamFlowShare(reach, reach) == 0.0 && insv::SeamFlowShare(-reach, reach) == 1.0);
    CHECK(insv::SeamFlowShare(20 * kDeg, reach) == 0.0);
    CHECK(near(insv::SeamFlowShare(0.0, reach), 0.5, 1e-12));
    CHECK(near(insv::SeamFlowShare(0.3 * reach, reach) + insv::SeamFlowShare(-0.3 * reach, reach),
               1.0, 1e-12));
    CHECK(insv::SeamFlowShare(0.2 * reach, reach) > insv::SeamFlowShare(0.6 * reach, reach));

    insv::FisheyeStitcher st;
    CHECK(st.configure(x5Calibration(), 3840, 3840, insv::StitchParams{}));
    if (!st.ready())
        return;
    const insv::SeamFlowGrid g = insv::SeamFlowGridFor(st);
    CHECK(g.valid() && g.cols == static_cast<int>(std::lround(360.0 * insv::kSeamFlowPxDeg)));
    CHECK(g.cols % 4 == 0); // the GPU's strip pass packs four columns
    CHECK(g.stripRad > 1.0 * kDeg && g.stripRad <= 11.0 * kDeg + 1e-6);
    CHECK(g.rows ==
          static_cast<int>(std::lround(2.0 * g.stripRad / kDeg * insv::kSeamFlowPxDeg)) + 1);
    CHECK(near(g.xOf(g.phiOf(17.0)), 17.0, 1e-9) && near(g.yOf(g.eOf(5.0)), 5.0, 1e-9));
    if (!g.valid())
        return;

    // F10 = (a, b) everywhere, F01 = (-a, -b).
    const float a = 0.004f, b = -0.002f;
    insv::SeamFlowField f;
    f.grid = g;
    f.v.resize(static_cast<size_t>(g.rows) * g.cols * 4);
    for (size_t k = 0; k < f.v.size(); k += 4) {
        f.v[k] = a;
        f.v[k + 1] = b;
        f.v[k + 2] = -a;
        f.v[k + 3] = -b;
    }
    float out[4];
    f.at(-0.5, -3.0, out); // wraps round in x, clamps in y
    CHECK(near(out[0], a, 1e-7) && near(out[3], -b, 1e-7));

    auto dist = [](const insv::Vec3& p, const insv::Vec3& q) {
        return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) +
                         (p.z - q.z) * (p.z - q.z));
    };
    // On the seam both lenses move half way, to the same place; past the
    // reach neither does.
    const insv::Vec3 onSeam = st.seamDir(1.0, 0.0);
    CHECK(dist(st.seamFlowRay(0, onSeam, f), st.seamDir(1.0 + 0.5 * a, 0.5 * b)) < 1e-6);
    CHECK(dist(st.seamFlowRay(1, onSeam, f), st.seamDir(1.0 - 0.5 * a, -0.5 * b)) < 1e-6);
    const insv::Vec3 far = st.seamDir(1.0, 9.0 * kDeg);
    const insv::Vec3 farW = st.seamFlowRay(0, far, f);
    CHECK(farW.x == far.x && farW.y == far.y && farW.z == far.z);
    // Half way to the reach on lens 0's side lens 0 moves less than half.
    const double e = 0.5 * reach, share = insv::SeamFlowShare(e, reach);
    CHECK(share > 0.0 && share < 0.5);
    CHECK(dist(st.seamFlowRay(0, st.seamDir(1.0, e), f), st.seamDir(1.0 + share * a, e + share * b)) <
          1e-6);

    // A zero field is the template, to the byte; a field moves the seam only.
    insv::Image l0, l1;
    l0.alloc(3840, 3840);
    l1.alloc(3840, 3840);
    for (int y = 0; y < 3840; ++y)
        for (int x = 0; x < 3840; ++x)
            for (int c = 0; c < 3; ++c) {
                const size_t i = (static_cast<size_t>(y) * 3840 + x) * 3 + c;
                l0.rgb[i] = static_cast<uint8_t>((x * 7 + y * 3 + c * 40) & 255);
                l1.rgb[i] = static_cast<uint8_t>((x * 5 + y * 11 + c * 70) & 255);
            }
    const insv::Image plain = st.renderEquirect(l0, l1, 1024);
    insv::SeamFlowField zero = f;
    std::fill(zero.v.begin(), zero.v.end(), 0.0f);
    insv::Image zeroEq, flowEq;
    zeroEq.alloc(1024, 512);
    flowEq.alloc(1024, 512);
    st.renderEquirect(l0, l1, zeroEq, nullptr, nullptr, nullptr, 0, nullptr, &zero);
    st.renderEquirect(l0, l1, flowEq, nullptr, nullptr, nullptr, 0, nullptr, &f);
    CHECK(zeroEq.rgb == plain.rgb);
    size_t changed = 0, changedFar = 0;
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 1024; ++x) {
            const size_t i = (static_cast<size_t>(y) * 1024 + x) * 3;
            if (flowEq.rgb[i] == plain.rgb[i])
                continue;
            ++changed;
            const double lat = (0.5 - (y + 0.5) / 512) * std::numbers::pi;
            const double lon = ((x + 0.5) / 1024 - 0.5) * 2.0 * std::numbers::pi;
            const insv::Vec3 dir = insv::apply(
                st.outRotation(), {std::cos(lat) * std::sin(lon), std::sin(lat), -std::cos(lat) * std::cos(lon)});
            const insv::Vec3& ax = st.seamAxis();
            changedFar += std::fabs(dir.x * ax.x + dir.y * ax.y + dir.z * ax.z) >= std::sin(reach);
        }
    CHECK(changed > 1000 && changedFar == 0);

    // The window: the mean of the last kSeamFlowFrames frames; another grid
    // starts it again.
    insv::SeamFlowWindow w;
    for (int k = 0; k <= insv::kSeamFlowFrames + 1; ++k) {
        insv::SeamFlowField fk = zero;
        std::fill(fk.v.begin(), fk.v.end(), static_cast<float>(k));
        w.add(std::move(fk));
    }
    CHECK(w.size() == insv::kSeamFlowFrames);
    insv::SeamFlowField mean;
    w.solve(mean);
    const double want = (2.0 + insv::kSeamFlowFrames + 1) / 2.0; // frames 2 .. N + 1
    CHECK(!mean.empty() && near(mean.v.front(), want, 1e-5) && near(mean.v.back(), want, 1e-5));
    insv::SeamFlowField other = zero;
    other.grid.rows -= 2;
    other.v.resize(static_cast<size_t>(other.grid.rows) * other.grid.cols * 4);
    w.add(std::move(other));
    CHECK(w.size() == 1);
    w.reset();
    w.solve(mean);
    CHECK(mean.empty());
}

void testSeamColourBlend()
{
    constexpr int B = insv::kSeamDepthBins, R = insv::kSeamCutRows;
    CHECK(insv::SeamColourFade(0.0) == 1.0 && insv::SeamColourFade(12.0 * kDeg) == 1.0);
    CHECK(near(insv::SeamColourFade(21.0 * kDeg), 0.5, 1e-9));
    CHECK(near(insv::SeamColourFade(-21.0 * kDeg), 0.5, 1e-9));
    CHECK(insv::SeamColourFade(30.0 * kDeg) == 0.0 && insv::SeamColourFade(-50.0 * kDeg) == 0.0);

    const double band = 5.0 * kDeg;
    auto truth = [](int b, int k) { return 0.08 * std::sin(2.0 * std::numbers::pi * b / B + k); };
    auto stats = std::make_unique<insv::SeamCutStats>();
    // Lens 0 at exp(L / 2) of the scene, lens 1 at exp(-L / 2); the scene's
    // level changes across the band and with the channel.
    auto fill = [&](const std::function<double(int, int)>& L) {
        for (int b = 0; b < B; ++b)
            for (int r = 0; r < R; ++r) {
                insv::SeamCutStats::Cell& c = stats->cell[b][r];
                c.n = 64;
                c.texture = 20;
                c.zsad = 2;
                for (int k = 0; k < 3; ++k) {
                    const double scene = 60.0 + 10.0 * r + 15.0 * k;
                    c.nColour[k] = 64;
                    c.mean[0][k] = static_cast<float>(scene * std::exp(0.5 * L(b, k)));
                    c.mean[1][k] = static_cast<float>(scene * std::exp(-0.5 * L(b, k)));
                }
            }
    };
    fill(truth);
    for (int b = 0; b < 10; ++b) // a sky clipped in blue
        for (int r = 0; r < R; ++r)
            stats->cell[b][r].nColour[2] = 0;
    const double scatter[R] = {0.3, -0.25, 0.1, -0.15, 0.35, -0.3, 0.2, -0.05, 0.25};
    for (int b = 60; b < 64; ++b) // flare on lens 0: its cells scattered
        for (int r = 0; r < R; ++r)
            for (int k = 0; k < 3; ++k)
                stats->cell[b][r].mean[0][k] *= static_cast<float>(std::exp(scatter[r]));
    float table[B * 3];
    CHECK(insv::MeasureSeamColour(*stats, band, insv::SeamColourTuning{}, table));
    double worst = 0, worstSky = 0, worstFlare = 0;
    for (int b = 0; b < B; ++b)
        for (int k = 0; k < 3; ++k) {
            const double d = std::fabs(table[b * 3 + k] - truth(b, k));
            (b < 10 && k == 2 ? worstSky : b >= 60 && b < 64 ? worstFlare : worst) =
                std::max(b < 10 && k == 2 ? worstSky : b >= 60 && b < 64 ? worstFlare : worst, d);
        }
    // The smoothing's own loss on a 128-bin sine, more next to the stretches
    // filled from one side; blue across the 28-degree clipped sky from its
    // two sides; the flare left out and filled from either side.
    CHECK(worst < 0.006);
    CHECK(worstSky < 0.015);
    CHECK(worstFlare < 0.008);

    // Bounded; a frame with nothing usable is no correction.
    fill([](int, int) { return 0.4; });
    CHECK(insv::MeasureSeamColour(*stats, band, insv::SeamColourTuning{}, table));
    CHECK(near(table[50 * 3 + 1], insv::kSeamColourMaxLog, 1e-6));
    for (int b = 0; b < B; ++b)
        for (int r = 0; r < R; ++r)
            stats->cell[b][r].n = 0;
    CHECK(!insv::MeasureSeamColour(*stats, band, insv::SeamColourTuning{}, table));
    CHECK(table[50 * 3 + 1] == 0.0f);

    // The window: a frame's table pools it and the frames in the
    // kSeamColourWindowMs before it, the nearer weighing more -- never the
    // frames after it. Frames 33 ms apart, the lenses 10 % apart in green up
    // to 1000 ms, then 0.
    insv::SeamColourWindow win;
    float t[insv::kSeamDepthBins * 3];
    auto step = [&](int64_t pts) {
        fill([pts](int, int k) { return k == 1 && pts <= 1000 ? 0.1 : 0.0; });
        win.add(pts, *stats, band);
    };
    step(0);
    CHECK(win.solve(0, t) && near(t[20 * 3 + 1], 0.1, 1e-6)); // one frame: its own
    for (int64_t pts = 33; pts <= 2000; pts += 33)
        step(pts);
    CHECK(win.solve(990, t) && near(t[20 * 3 + 1], 0.1, 1e-6));  // later frames don't count
    CHECK(win.solve(2000, t) && near(t[20 * 3 + 1], 0.0, 1e-6)); // all of its second at 0
    CHECK(win.solve(1500, t)); // half a second on: the older half, weighing less
    CHECK(t[20 * 3 + 1] > 0.015 && t[20 * 3 + 1] < 0.04);
    CHECK(near(t[20 * 3], 0.0, 1e-6));
    win.dropBefore(1500);
    CHECK(!win.solve(1400, t)); // none left up to it
    CHECK(win.solve(2000, t) && near(t[20 * 3 + 1], 0.0, 1e-6));
    // A frame earlier than the last (a seek back) starts afresh.
    fill([](int, int) { return 0.05; });
    win.add(100, *stats, band);
    CHECK(win.solve(100, t) && near(t[20 * 3 + 1], 0.05, 1e-6));
    win.reset();
    CHECK(win.empty() && !win.solve(100, t) && t[20 * 3 + 1] == 0.0f);

    // The CPU stitch: uniform grey lenses, green 10 % apart in the table
    // everywhere. Lens 0 alone (past the band) gets darker in green by
    // exp(-0.05) times the fade, lens 1 brighter; past 30 deg nothing moves.
    insv::Image l0, l1;
    l0.alloc(800, 800);
    l1.alloc(800, 800);
    std::fill(l0.rgb.begin(), l0.rgb.end(), uint8_t(100));
    std::fill(l1.rgb.begin(), l1.rgb.end(), uint8_t(100));
    insv::FisheyeStitcher st;
    CHECK(st.configure(x5Calibration(), 800, 800, insv::StitchParams{}));
    std::vector<float> green(B * 3, 0.0f);
    for (int b = 0; b < B; ++b)
        green[b * 3 + 1] = 0.1f;
    double L[3];
    st.seamColourLog(st.seamDir(1.0, 3.0 * kDeg), green.data(), L);
    CHECK(near(L[1], 0.1, 1e-6) && L[0] == 0.0);
    st.seamColourLog(st.seamDir(1.0, -21.0 * kDeg), green.data(), L);
    CHECK(near(L[1], 0.05, 1e-6));
    insv::Image plain, matched;
    plain.alloc(256, 128);
    matched.alloc(256, 128);
    st.renderEquirect(l0, l1, plain);
    st.renderEquirect(l0, l1, matched, nullptr, nullptr, nullptr, 0, green.data());
    int checked = 0, wrong = 0;
    const double bandRad = st.gpuParams().bandRad;
    for (int y = 0; y < 128; y += 3)
        for (int x = 0; x < 256; x += 3) {
            const double lat = (0.5 - (y + 0.5) / 128) * std::numbers::pi;
            const double lon = ((x + 0.5) / 256 - 0.5) * 2.0 * std::numbers::pi;
            const insv::Vec3 dir = insv::apply(
                st.outRotation(),
                insv::Vec3{std::cos(lat) * std::sin(lon), std::sin(lat), -std::cos(lat) * std::cos(lon)});
            double e, t;
            int i0, i1;
            st.seamPosition(dir, e, i0, i1, t);
            const size_t o = (static_cast<size_t>(y) * 256 + x) * 3;
            if (plain.rgb[o + 1] == 0)
                continue; // outside both circles
            const int g = matched.rgb[o + 1], g0 = plain.rgb[o + 1];
            if (std::fabs(e) < bandRad + 0.5 * kDeg)
                continue; // blended from both
            const double want = g0 * std::exp((e > 0 ? -0.05 : 0.05) * insv::SeamColourFade(e));
            ++checked;
            if (std::abs(g - want) > 0.75 || matched.rgb[o] != plain.rgb[o])
                ++wrong;
        }
    CHECK(checked > 1000 && wrong == 0);

    // The veiling light it takes out: lens 0's (> 0) in the band, half of it
    // half way through the ramp past it, none beyond; none at all leaves the
    // stitch as it was, bit for bit.
    std::vector<float> veil(B * 3, 0.0f);
    insv::Image unveiled;
    unveiled.alloc(256, 128);
    st.renderEquirect(l0, l1, unveiled, nullptr, nullptr, nullptr, 0, nullptr, nullptr, veil.data());
    CHECK(unveiled.rgb == plain.rgb);
    for (int b = 0; b < B; ++b)
        veil[b * 3 + 1] = 0.04f;
    double vl[2][3];
    st.seamVeil(st.seamDir(1.0, 3.0 * kDeg), veil.data(), vl);
    CHECK(near(vl[0][1], 0.04, 1e-6) && vl[1][1] == 0.0 && vl[0][0] == 0.0);
    const double rampRad = insv::kLensColourLocalRampDeg * kDeg;
    st.seamVeil(st.seamDir(1.0, -(bandRad + 0.5 * rampRad)), veil.data(), vl);
    CHECK(near(vl[0][1], 0.02, 1e-6));
    st.seamVeil(st.seamDir(1.0, bandRad + rampRad + 0.1 * kDeg), veil.data(), vl);
    CHECK(vl[0][1] == 0.0);
    insv::Image veiled;
    veiled.alloc(256, 128);
    st.renderEquirect(l0, l1, veiled, nullptr, nullptr, nullptr, 0, nullptr, nullptr, veil.data());
    int darker = 0, moved = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 256; ++x) {
            const double lat = (0.5 - (y + 0.5) / 128) * std::numbers::pi;
            const double lon = ((x + 0.5) / 256 - 0.5) * 2.0 * std::numbers::pi;
            const insv::Vec3 dir = insv::apply(
                st.outRotation(),
                insv::Vec3{std::cos(lat) * std::sin(lon), std::sin(lat), -std::cos(lat) * std::cos(lon)});
            double e, t;
            int i0, i1;
            st.seamPosition(dir, e, i0, i1, t);
            const size_t o = (static_cast<size_t>(y) * 256 + x) * 3;
            if (plain.rgb[o + 1] == 0)
                continue;
            if (e > 0 && e < bandRad)
                darker += veiled.rgb[o + 1] < plain.rgb[o + 1] - 3;
            if (std::fabs(e) > bandRad + rampRad || veiled.rgb[o] != plain.rgb[o])
                moved += veiled.rgb[o + 1] != plain.rgb[o + 1] || veiled.rgb[o] != plain.rgb[o];
        }
    CHECK(darker > 50 && moved == 0);
}

// The local correction round the seam: a stretch where the lenses differ
// (a low sun's veil in one lens) gets it, the rest of the seam doesn't, nor
// does a stretch with no data of its own; it follows flare frames.
void testLensColourLocal()
{
    constexpr int B = insv::kSeamDepthBins, R = insv::kSeamCutRows;
    const double band = 5.0 * kDeg;
    auto stats = std::make_unique<insv::SeamCutStats>();
    auto costs = farCosts();
    auto fill = [&](double veil) {
        for (int b = 0; b < B; ++b)
            for (int r = 0; r < R; ++r) {
                insv::SeamCutStats::Cell& c = stats->cell[b][r];
                c.n = 64;
                c.texture = 20;
                c.zsad = 2;
                for (int k = 0; k < 3; ++k) // nothing usable there (clipped)
                    c.nColour[k] = b < 20 ? 0.0f : 64.0f;
                for (int k = 0; k < 3; ++k) {
                    c.mean[1][k] = 120;
                    c.mean[0][k] = 120;
                }
                if (b >= 30 && b < 50) // lens 1 veiled: lens 0 darker
                    for (int k = 0; k < 3; ++k)
                        c.mean[0][k] = static_cast<float>(120 * std::exp(-veil));
            }
    };
    fill(0.2);
    insv::SeamWindow w;
    w.configure(0.032, band);
    w.add(1000, *costs, *stats);
    insv::SeamState s;
    w.solve(1000, s);
    const insv::SeamBlend& bl = s.blend;
    CHECK(near(bl.local[40][1], -0.2, 0.01));
    CHECK(near(bl.local[90][1], 0.0, 1e-4));
    CHECK(bl.local[5][1] == 0.0f);         // no data of its own, out of reach: none
    CHECK(near(bl.colour[1], 0.0, 1e-4));  // the whole seam: the rest agrees
    // Over the frames within ~40 ms: with one 10 ms later that has none,
    // about half.
    fill(0.0);
    w.add(1010, *costs, *stats);
    w.solve(1010, s);
    CHECK(bl.local[40][1] < -0.08 && bl.local[40][1] > -0.12);
    // A veil over most of the seam, growing along it (flare towards the
    // sun): the whole-seam residual leaves those frames out, the local
    // correction follows them; where the lenses are further apart than any
    // gain (past bin ~70) it is taken for light lens 1 adds instead.
    for (int b = 20; b < B; ++b)
        for (int r = 0; r < R; ++r)
            for (int k = 0; k < 3; ++k)
                stats->cell[b][r].mean[0][k] = static_cast<float>(120 * std::exp(-0.05 - 0.004 * (b - 20)));
    for (int t = 1043; t < 2000; t += 33)
        w.add(t, *costs, *stats);
    w.solve(1043 + 33 * 28, s);
    CHECK(near(bl.colour[1], 0.0, 1e-4));
    CHECK(near(bl.local[50][1], -0.05 - 0.004 * 30, 0.02) && bl.veil[50][1] == 0.0f);
    CHECK(bl.veil[110][1] < -0.05f && bl.veil[110][0] < -0.05f && std::fabs(bl.local[110][1]) < 0.05);
}

// Veiling light in lens 1 over bins 30..49 (light added, the same at every
// level, in linear light), the lenses 5 % apart in gain everywhere: the
// veil is found where it is and nowhere else, taken out of lens 1, and the
// colour match then sees only the gain. Cells all about as bright can't
// tell a veil from a gain: none.
void testLensVeil()
{
    constexpr int B = insv::kSeamDepthBins, R = insv::kSeamCutRows;
    const double band = 5.0 * kDeg;
    const double veil[3] = {0.08, 0.06, 0.04};
    auto enc = [](double lin) { return 255.0 * std::pow(lin, 1.0 / insv::kLensVeilGamma); };
    auto stats = std::make_unique<insv::SeamCutStats>();
    auto fill = [&](bool levels) {
        for (int b = 0; b < B; ++b)
            for (int r = 0; r < R; ++r) {
                insv::SeamCutStats::Cell& c = stats->cell[b][r];
                c.n = 64;
                c.texture = 20;
                c.zsad = 2;
                const double clean = levels ? 0.03 + 0.07 * ((r + b) % R) : 0.2;
                for (int k = 0; k < 3; ++k) {
                    c.nColour[k] = 64;
                    c.mean[0][k] = static_cast<float>(1.05 * enc(clean));
                    c.mean[1][k] = static_cast<float>(enc(clean + (b >= 30 && b < 50 ? veil[k] : 0.0)));
                }
            }
    };
    fill(true);
    std::vector<float> v(B * 3);
    insv::LensVeilPerBin(*stats, v.data());
    for (int k = 0; k < 3; ++k) {
        CHECK(near(v[40 * 3 + k], -veil[k], 0.01) && near(v[35 * 3 + k], -veil[k], 0.01));
        CHECK(v[90 * 3 + k] == 0.0f && v[10 * 3 + k] == 0.0f);
    }
    const insv::SeamState s = solveOne(*stats, band);
    const insv::SeamBlend& bl = s.blend;
    CHECK(near(bl.veil[40][1], -veil[1], 0.01) && bl.veil[90][1] == 0.0f);
    CHECK(near(bl.colour[1], std::log(1.05), 0.01));
    CHECK(std::fabs(bl.local[40][1]) < 0.02 && std::fabs(bl.local[90][1]) < 0.01);

    // The new stitcher's seam colour blend finds it the same way and matches
    // the colour with it taken out: the 5 % gain alone, there as elsewhere.
    // The veil is held kSeamVeilWindowMs: a frame 100 ms on without one has
    // none.
    insv::SeamColourWindow win;
    win.add(0, *stats, band);
    std::vector<float> table(B * 3), wv(B * 3);
    CHECK(win.solve(0, table.data(), wv.data()));
    for (int k = 0; k < 3; ++k)
        CHECK(near(wv[40 * 3 + k], v[40 * 3 + k], 1e-6) && wv[90 * 3 + k] == 0.0f);
    CHECK(near(table[40 * 3 + 1], std::log(1.05), 0.01) && near(table[90 * 3 + 1], std::log(1.05), 0.01));
    for (int b = 0; b < B; ++b)
        for (int r = 0; r < R; ++r)
            for (int k = 0; k < 3; ++k)
                stats->cell[b][r].mean[1][k] = stats->cell[b][r].mean[0][k] / 1.05f;
    win.add(100, *stats, band);
    CHECK(win.solve(100, table.data(), wv.data()) && wv[40 * 3 + 1] == 0.0f);

    // One level everywhere, lens 1 brighter there: a gain, not a veil.
    fill(false);
    insv::LensVeilPerBin(*stats, v.data());
    CHECK(v[40 * 3 + 1] == 0.0f);
}

// A channel clipped in a stretch (a bright sky's blue) leaves the others
// usable there: red is matched where only blue is clipped; blue isn't
// given the neighbouring stretches' correction.
void testLensColourClippedChannel()
{
    constexpr int B = insv::kSeamDepthBins, R = insv::kSeamCutRows;
    const double band = 5.0 * kDeg;
    auto stats = std::make_unique<insv::SeamCutStats>();
    for (int b = 0; b < B; ++b)
        for (int r = 0; r < R; ++r) {
            insv::SeamCutStats::Cell& c = stats->cell[b][r];
            c.n = 64;
            c.texture = 20;
            c.zsad = 2;
            const bool sky = b >= 100;
            for (int k = 0; k < 3; ++k) {
                c.nColour[k] = sky && k == 2 ? 0.0f : 64.0f;
                c.mean[1][k] = 120;
                c.mean[0][k] = sky && k != 2 ? 132.0f : (b >= 90 && b < 100 && k == 2 ? 150.0f : 120.0f);
            }
        }
    const insv::SeamState s = solveOne(*stats, band);
    const insv::SeamBlend& bl = s.blend;
    CHECK(near(bl.local[110][0], std::log(1.1), 0.01));
    CHECK(near(bl.local[95][2], std::log(1.25), 0.01));
    CHECK(bl.local[110][2] == 0.0f);
}

// The local correction is measured where the lenses meet: a sky the seam
// cuts through agrees, the trees further across the band differ (a small
// offset in their shadows); matched on the trees, the sky would split.
void testLensColourWhereTheyMeet()
{
    constexpr int B = insv::kSeamDepthBins, R = insv::kSeamCutRows;
    const double band = 6.0 * kDeg;
    auto stats = std::make_unique<insv::SeamCutStats>();
    for (int b = 0; b < B; ++b)
        for (int r = 0; r < R; ++r) {
            insv::SeamCutStats::Cell& c = stats->cell[b][r];
            c.n = 64;
            c.texture = 20;
            c.zsad = 2;
            for (int k = 0; k < 3; ++k) {
                c.nColour[k] = 64;
                c.mean[1][k] = 150;
                c.mean[0][k] = r <= 2 ? 150.0f : static_cast<float>(150 * 1.1);
            }
        }
    const double rowStep = 2.0 * insv::SeamCutRowMax(band) / (R - 1);
    std::vector<float> meet(B, static_cast<float>(insv::SeamCutRowE(0, band)));
    std::vector<float> reach(B, static_cast<float>(rowStep));
    std::vector<float> v(B * 3), w(B * 3);
    const double none[3] = {0, 0, 0};
    insv::LensColourPerBin(*stats, band, insv::LensColour{}, none, meet.data(), reach.data(),
                           v.data(), w.data());
    CHECK(std::fabs(v[10 * 3 + 1]) < 0.005 && w[10 * 3 + 1] > 64);
    insv::LensColourPerBin(*stats, band, insv::LensColour{}, none, nullptr, nullptr, v.data(),
                           w.data());
    CHECK(near(v[10 * 3 + 1], std::log(1.1), 0.005));
}

// FitLensColour on synthetic statistics: a known constant and slope across
// the band, with frames of flare mixed in, clipped and disagreeing cells.
void testFitLensColour()
{
    constexpr int B = insv::kSeamDepthBins, R = insv::kSeamCutRows;
    const double band = 7.5 * kDeg;
    const double a[3] = {0.02, -0.01, -0.06}, slope[3] = {0.004, 0.0, -0.008}; // per degree
    std::vector<std::unique_ptr<insv::SeamCutStats>> frames;
    for (int f = 0; f < 24; ++f) {
        auto s = std::make_unique<insv::SeamCutStats>();
        const bool flare = f % 6 == 2; // 4 of 24
        for (int b = 0; b < B; ++b)
            for (int r = 0; r < R; ++r) {
                insv::SeamCutStats::Cell& c = s->cell[b][r];
                c.n = 64;
                for (float& n : c.nColour)
                    n = 64;
                c.texture = 20;
                c.zsad = (b * 7 + r + f) % 23 == 0 ? 30.0f : 4.0f; // some disagree
                const double e = insv::SeamCutRowE(r, band) / kDeg;
                // Scene colour varies by bin and frame; a little noise per cell.
                const double base = 60.0 + 100.0 * ((b * 13 + f * 5) % 17) / 17.0;
                for (int k = 0; k < 3; ++k) {
                    const double noise = 0.01 * (((b * 31 + r * 17 + k * 7 + f * 3) % 11) - 5) / 5.0;
                    double L = a[k] + slope[k] * e + noise;
                    if (flare && b < 80)
                        L -= 0.1 + 0.03 * (b % 10); // the up lens's veil, uneven, on most of the seam
                    if (c.zsad > 10)
                        L += 0.4; // what disagrees is off too, and must not count
                    c.mean[1][k] = static_cast<float>(base);
                    c.mean[0][k] = static_cast<float>(base * std::exp(L));
                }
                if ((b + r) % 19 == 0) // clipped in lens 0
                    c.mean[0][1] = 245.0f;
            }
        frames.push_back(std::move(s));
    }
    std::vector<const insv::SeamCutStats*> ptrs;
    for (const auto& s : frames)
        ptrs.push_back(s.get());
    const insv::LensColourFit r = insv::FitLensColour(ptrs, band);
    std::printf("lens colour: %s\n", r.message.c_str());
    CHECK(r.ok && r.frames == 20 && r.flareFrames == 4);
    for (int k = 0; k < 3; ++k)
        CHECK(near(r.colour.logRatio[k], a[k], 0.004) && near(r.colour.slope[k], slope[k], 0.0005));
    CHECK(r.colour.spread < 0.2);

    // The residual of an ordinary frame against it is ~0; a flare frame's
    // isn't taken.
    double res[3];
    CHECK(insv::LensColourResidual(*frames[0], band, r.colour, res));
    for (int k = 0; k < 3; ++k)
        CHECK(near(res[k], 0.0, 0.006));
    CHECK(!insv::LensColourResidual(*frames[2], band, r.colour, res));

    // A difference on one side of the seam in every frame (3 of the 8
    // stretches) doesn't move the clip's value.
    for (const auto& s : frames)
        for (int b = 0; b < 48; ++b)
            for (int r = 0; r < R; ++r)
                for (int k = 0; k < 3; ++k)
                    s->cell[b][r].mean[0][k] *= 0.85f;
    ptrs.clear();
    for (const auto& s : frames)
        ptrs.push_back(s.get());
    const insv::LensColourFit sided = insv::FitLensColour(ptrs, band);
    std::printf("lens colour, one side darker: %s\n", sided.message.c_str());
    CHECK(sided.ok);
    for (int k = 0; k < 3; ++k)
        CHECK(near(sided.colour.logRatio[k], a[k], 0.004));

    // Blue clipped over half the seam: red and green still from all of it,
    // blue from the rest.
    for (const auto& st : frames)
        for (int b = 0; b < 64; ++b)
            for (int r = 0; r < R; ++r)
                st->cell[b][r].nColour[2] = 0;
    const insv::LensColourFit clipped = insv::FitLensColour(ptrs, band);
    std::printf("lens colour, blue clipped on half the seam: %s\n", clipped.message.c_str());
    CHECK(clipped.ok && clipped.cells == sided.cells);
    for (int k = 0; k < 3; ++k)
        CHECK(near(clipped.colour.logRatio[k], a[k], 0.006) && near(clipped.colour.slope[k], slope[k], 0.001));

    // Too few frames: no fit.
    ptrs.resize(2);
    CHECK(!insv::FitLensColour(ptrs, band).ok);
}

// The cut statistics on synthetic lens pictures: the lenses agree on far
// content; on content 0.33 m away only once the depth table says so.
void testSeamCutStats()
{
    const insv::InsvCalibration cal = x5Calibration();
    constexpr int kSize = 1536;
    insv::StitchParams p;
    p.depthSeam = true;
    insv::FisheyeStitcher st;
    CHECK(st.configure(cal, kSize, kSize, p));
    const double nearQ = 3.0;
    const insv::Image l0 = seamLensPicture(st, 0, kSize, 1, 140.0, 220.0, nearQ);
    const insv::Image l1 = seamLensPicture(st, 1, kSize, 1, 140.0, 220.0, nearQ);
    // Bins whose patch lies wholly inside / outside the near stretch, by
    // lens 0's azimuth (seamLensPicture's) at the patch centre.
    const insv::Mat3& w2l = st.lensProjection(0).worldToLens;
    auto nearBin = [&](int b, int r) {
        double phi, e, lo = 400, hi = -400;
        for (int s : {0, insv::kSeamCutPatchAz - 1}) {
            insv::SeamCutSample(b, r, s, st.gpuParams().bandRad, phi, e);
            const insv::Vec3 d = insv::apply(w2l, st.seamDir(phi, e));
            double a = std::atan2(d.y, d.x) / kDeg;
            if (a < 0)
                a += 360;
            lo = std::min(lo, a), hi = std::max(hi, a);
        }
        return hi - lo > 180 ? 0 : (lo > 142 && hi < 218) ? 1 : (hi < 138 || lo > 222) ? -1 : 0;
    };
    auto disagreement = [&](const float* table, int want) {
        auto stats = std::make_unique<insv::SeamCutStats>();
        insv::MeasureSeamCutStats(st, l0, l1, table, *stats);
        double sum = 0;
        int n = 0;
        for (int b = 0; b < insv::kSeamDepthBins; ++b)
            for (int r = 0; r < insv::kSeamCutRows; ++r) {
                const insv::SeamCutStats::Cell& c = stats->cell[b][r];
                if (nearBin(b, r) != want || c.n < 32)
                    continue;
                sum += c.zsad / (c.texture + 4.0);
                ++n;
            }
        CHECK(n > 50);
        return sum / std::max(n, 1);
    };
    float far[insv::kSeamDepthBins] = {}, depth[insv::kSeamDepthBins];
    for (int b = 0; b < insv::kSeamDepthBins; ++b)
        depth[b] = nearBin(b, insv::kSeamCutRows / 2) >= 0 ? static_cast<float>(nearQ) : 0.0f;
    const double farAtFar = disagreement(far, -1), nearAtFar = disagreement(far, 1);
    const double nearAtDepth = disagreement(depth, 1);
    std::printf("seam cut: disagreement far %.3f, near with the far table %.3f, with its depth %.3f\n",
                farAtFar, nearAtFar, nearAtDepth);
    CHECK(farAtFar < 0.2 && nearAtDepth < 0.2);
    CHECK(nearAtFar > 0.5);

    // Lens 1 a fifth darker: the lens colour fit finds it from the lens
    // pictures, and the stitched colours of the two lenses then meet.
    insv::Image dark = l1;
    for (auto& v : dark.rgb)
        v = static_cast<uint8_t>(std::lround(v * 0.8));
    auto stats = std::make_unique<insv::SeamCutStats>();
    insv::MeasureSeamCutStats(st, l0, dark, depth, *stats);
    const std::vector<const insv::SeamCutStats*> three(3, stats.get());
    const insv::LensColourFit fit = insv::FitLensColour(three, st.gpuParams().bandRad);
    std::printf("seam cut: lens colour %s\n", fit.message.c_str());
    CHECK(fit.ok);
    for (int k = 0; k < 3; ++k)
        CHECK(near(std::exp(fit.colour.logRatio[k]), 1.25, 0.02) && std::fabs(fit.colour.slope[k]) < 0.002);
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
        CHECK(!thumb->flat);
        CHECK(thumb->coding.matrix == insv::YcbcrMatrix::Bt601 && !thumb->coding.fullRange);
    }
    std::filesystem::remove(ok);

    // The Antigravity A1's layout, shrunk: a 64-byte header, then a flat
    // 6x4 view in rows 8 bytes apart; the padding is left out.
    {
        const int aw = 6, ah = 4, stride = 8;
        std::vector<uint8_t> a1(64 + stride * ah * 3 / 2, 0xee);
        auto put = [&](size_t at, uint32_t v) {
            for (int i = 0; i < 4; ++i) a1[at + i] = uint8_t(v >> (8 * i));
        };
        std::fill(a1.begin(), a1.begin() + 64, 0);
        put(4, 64);
        put(8, uint32_t(stride * ah * 3 / 2));
        put(24, aw);
        put(28, ah);
        put(32, stride);
        for (int row = 0; row < ah * 3 / 2; ++row)
            for (int x = 0; x < aw; ++x)
                a1[size_t(64 + row * stride + x)] = uint8_t(row * 10 + x);
        const std::string path = writeTrailerFile("insv_test_thumb_a1.insv", 0x200, a1);
        const auto t = insv::ReadInsvThumbnail(path);
        CHECK(t.has_value());
        if (t) {
            CHECK(t->width == aw && t->height == ah && t->flat);
            CHECK(t->coding.matrix == insv::YcbcrMatrix::Bt709 && t->coding.fullRange);
            CHECK(t->nv12.size() == size_t(aw * ah * 3 / 2));
            bool rows = t->nv12.size() == size_t(aw * ah * 3 / 2);
            for (int row = 0; rows && row < ah * 3 / 2; ++row)
                for (int x = 0; x < aw; ++x)
                    rows = rows && t->nv12[size_t(row * aw + x)] == uint8_t(row * 10 + x);
            CHECK(rows);
        }
        std::filesystem::remove(path);

        // A picture size that doesn't match the rows: nothing.
        put(8, uint32_t(stride * ah * 3 / 2 - 1));
        const std::string bad = writeTrailerFile("insv_test_thumb_a1_bad.insv", 0x200, a1);
        CHECK(!insv::ReadInsvThumbnail(bad).has_value());
        std::filesystem::remove(bad);
    }

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
    m.pbVarint(7, 20250904193105ull);
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

// Record 0x700 decoded: acquired fixes only, each once although the file
// repeats it every 100 ms, the hemispheres as signs, and field 98.3 to line
// it up with the video.
void testGps()
{
    using testtrailer::gpsRecord;
    constexpr uint64_t t0 = 1757003465000ull;
    Bytes gps;
    for (int i = 0; i < 10; ++i) // no fix yet
        gps.raw(gpsRecord(t0 - 1000, 'V', 0, 0, 0, 0, 0, 0, 0));
    for (int i = 0; i < 30; ++i) { // three fixes, each over 10 records
        const int k = i / 10;
        gps.raw(gpsRecord(t0 + uint64_t(k) * 1000 + 999, 'A', 33.5 + k * 0.001, 'S',
                          70.25, k == 2 ? 'O' : 'W', 10.0 + k, 350.0 + k * 10.0, -2.5));
    }
    for (int i = 0; i < 25; ++i) // the signal lost: the last fix repeats
        gps.raw(gpsRecord(t0 + 2999, 'A', 33.502, 'S', 70.25, 'W', 12.0, 10.0, -2.5));
    gps.raw(gpsRecord(t0 + 1500, 'A', 1, 'N', 1, 'E', 0, 0, 0)); // out of order
    gps.raw(gpsRecord(t0 + 9000, 'A', 33.6, 'N', 70.3, 'E', 0.0, -1.0, 4.0));
    gps.raw(std::vector<uint8_t>(20, 0)); // a cut-off record, ignored

    Bytes meta;
    meta.pbString(2, "Insta360 X5");
    Bytes times; // field 98: first / last frame device us, Unix ms, frames
    times.pbVarint(1, 2468081984ull).pbVarint(2, 2510815317ull).pbVarint(3, t0 + 871);
    times.pbVarint(4, t0 + 43604).pbVarint(5, 1282);
    meta.pbBytes(98, times.b);

    const std::string path =
        writeX5Trailer("insv_test_gps.insv", {{0x101, meta.b}, {0x700, gps.b}});
    const auto g = insv::ReadInsvGps(path);
    CHECK(g && g->first_frame_unix_ms == int64_t(t0 + 871));
    CHECK(g && g->fixes.size() == 4);
    if (g && g->fixes.size() == 4) {
        const insv::InsvGpsFix& a = g->fixes[0];
        CHECK(a.unix_ms == int64_t(t0 + 999));
        CHECK(a.latitude == -33.5 && a.longitude == -70.25);
        CHECK(a.speed_mps == 10.0 && a.heading_deg == 350.0 && a.altitude_m == -2.5);
        CHECK(g->fixes[2].longitude == -70.25); // 'O' is west
        CHECK(g->fixes[3].unix_ms == int64_t(t0 + 9000));
        CHECK(g->fixes[3].latitude == 33.6 && g->fixes[3].longitude == 70.3);
        CHECK(g->fixes[3].heading_deg == -1.0); // standing still: kept as written
    }
    // The count for the Media information panel is unchanged by the decode.
    const auto t = insv::ReadInsvTelemetry(path);
    CHECK(t && t->gps_records == 67 && t->gps_fixes == 57);
    std::filesystem::remove(path);

    // No field 98 (Insta360's trimmed exports), no GPS record, no trailer.
    const std::string bare = writeX5Trailer("insv_test_gps_bare.insv",
                                            {{0x101, metadataRecord(false)}, {0x700, gps.b}});
    const auto b = insv::ReadInsvGps(bare);
    CHECK(b && b->first_frame_unix_ms == 0 && b->fixes.size() == 4);
    std::filesystem::remove(bare);
    const std::string none =
        writeX5Trailer("insv_test_gps_none.insv", {{0x101, metadataRecord(false)}});
    const auto n = insv::ReadInsvGps(none);
    CHECK(n && n->fixes.empty());
    std::filesystem::remove(none);
    CHECK(!insv::ReadInsvGps("/nonexistent/clip.insv"));
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
        CHECK(notes->creation_time == 20250904193105ull);
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

// An Antigravity A1-shaped trailer: the offsets table is the only index, each
// record starts a slot and no header follows it, so the walker takes every
// record's id and size from its table entry (docs/formats/antigravity-a1.md).
// Slow motion and timelapse (docs/formats/antigravity-a1.md, *Slow motion
// and timelapse*): the rates of fields 195 / 196, the interval of fields 30 /
// 59 and the frame times of record 0x600, and the capture scale they give.
void testCaptureTiming()
{
    auto rate = [](uint64_t num, uint64_t den) {
        Bytes r;
        r.pbVarint(1, num).pbVarint(2, den);
        return r.b;
    };
    // 4K Slow Motion: captured at 100, stored at 29.97.
    {
        Bytes m;
        m.pbBytes(195, rate(100000, 1000)).pbBytes(196, rate(29970, 1000));
        const std::string path = writeX5Trailer("insv_test_slowmo.insv", {{0x101, m.b}});
        const auto t = insv::ReadInsvTelemetry(path);
        CHECK(t && t->capture_fps == 100.0 && t->stored_fps == 29.97);
        CHECK(t && t->timelapse_interval_ms == 0.0 && t->frame_timestamps.empty());
        CHECK(t && std::fabs(insv::CaptureScale(*t, 29.97) - 0.2997) < 1e-12);
        std::filesystem::remove(path);
    }
    // Timelapse, one frame every 2 s at 29.97: the frames' own times (2.002 s
    // apart, as the A1 writes them) win over the interval field.
    {
        Bytes m;
        m.pbBytes(195, rate(30000, 1001)).pbBytes(196, rate(30000, 1001));
        m.pbVarint(30, 2000).pbVarint(59, 1);
        Bytes stamps;
        for (int i = 0; i < 150; ++i)
            stamps.u64(477724154ull + uint64_t(i) * 2002000ull);
        const std::string path =
            writeX5Trailer("insv_test_timelapse.insv", {{0x101, m.b}, {0x600, stamps.b}});
        const auto t = insv::ReadInsvTelemetry(path);
        CHECK(t && t->timelapse_interval_ms == 2000.0 && t->frame_timestamps.size() == 150);
        CHECK(t && t->frame_timestamps.front() == 477724154ull);
        CHECK(t && std::fabs(insv::CaptureScale(*t, 29.97) - 60.0) < 1e-9);
        std::filesystem::remove(path);
    }
    // Without frame times the interval gives it; without field 196 the
    // video's rate stands in; a normal clip (equal rates, or none) is 1.
    {
        insv::InsvTelemetry t;
        t.timelapse_interval_ms = 2000.0;
        CHECK(std::fabs(insv::CaptureScale(t, 30000.0 / 1001.0) - 2.0 * 30000.0 / 1001.0) < 1e-12);
        t.timelapse_interval_ms = 0.0;
        t.capture_fps = t.stored_fps = 60000.0 / 1001.0;
        CHECK(insv::CaptureScale(t, 59.94) == 1.0);
        CHECK(insv::CaptureScale(insv::InsvTelemetry{}, 29.97) == 1.0);
        CHECK(insv::CaptureScale(insv::InsvTelemetry{}, 0.0) == 1.0);
    }
}

void testA1Trailer()
{
    std::printf("  Antigravity A1 trailer\n");
    Bytes meta;
    meta.pbString(1, "A1000000000000000001").pbString(2, "Antigravity A1");
    meta.pbString(3, "10.0.397.3_5.5").pbString(54, kOneX2V3);
    meta.pbVarint(24, 1000300);
    meta.pbDouble(25, 21.247);
    for (float v : {0.5f, 0.5f, -0.5f, 0.5f}) // field 69, unpacked as the A1 writes it
        meta.pbFloat(69, v);
    // Attitude: x, y, z, w then velocity N, E, D; the third quaternion isn't
    // unit and is dropped.
    Bytes attitude;
    const float s30 = 0.25881904f, c30 = 0.96592583f; // 30 deg about body y
    attitude.u64(1000000).f32(0).f32(0).f32(0).f32(1).f32(1.5f).f32(-2.0f).f32(0.25f);
    attitude.u64(1020000).f32(0).f32(s30).f32(0).f32(c30).f32(0).f32(0).f32(0);
    attitude.u64(1040000).f32(0).f32(0).f32(0).f32(0.5f).f32(0).f32(0).f32(0);
    Bytes imu; // 56-byte doubles, 1 ms apart
    for (int i = 0; i < 4; ++i) {
        imu.u64(1000000 + uint64_t(i) * 1000);
        for (double v : {0.0, 0.0, 1.0, 0.1, 0.2, 0.3}) imu.f64(v);
    }
    Bytes exposure;
    exposure.u64(1000300).f64(0.001).u64(1033667).f64(0.001);

    const std::string path = (std::filesystem::temp_directory_path() / "insv_test_a1.insv").string();
    {
        std::ofstream f(path, std::ios::binary);
        const std::vector<uint8_t> video(64, 0x11); // stands in for the MP4 boxes
        const std::vector<uint8_t> t =
            insvTrailer({{0x101, meta.b}, {0x300, imu.b}, {0x400, exposure.b},
                         {0x2500, attitude.b}, {0x3800, std::vector<uint8_t>(40, 0x5a)}},
                        true, 256);
        f.write(reinterpret_cast<const char*>(video.data()), std::streamsize(video.size()));
        f.write(reinterpret_cast<const char*>(t.data()), std::streamsize(t.size()));
    }

    const auto notes = insv::ReadInsvMakerNotes(path);
    CHECK(notes && notes->model == "Antigravity A1" && notes->firmware == "10.0.397.3_5.5");
    CHECK(notes && notes->offset_v3 == kOneX2V3);
    bool known = false;
    const insv::InsvModel& model = insv::FindInsvModel("Antigravity A1", &known);
    CHECK(known && std::string(model.maker) == "Antigravity" && model.telemetryTickUs == 1.0);
    CHECK(!model.secondLensToken && !model.clipToken);
    CHECK(std::string(model.captureMode(3840)) == "8K");
    CHECK(std::string(model.captureMode(2624)) == "5.2K");
    CHECK(model.mountDeg[0] == -90.0 && model.mountDeg[1] == 90.0 && model.mountDeg[2] == 0.0);
    CHECK(std::string(model.imuAxes) == "-x,y,-z");
    // Insta360's cameras keep the stitch's default: lens 0's opposite forward.
    const insv::StitchParams defaults;
    for (const insv::InsvModel& m : insv::InsvModels())
        if (std::string(m.maker) == "Insta360")
            CHECK(m.mountDeg[0] == defaults.outYawDeg && m.mountDeg[1] == defaults.outPitchDeg &&
                  m.mountDeg[2] == defaults.outRollDeg);
    const auto t = insv::ReadInsvTelemetry(path);
    CHECK(t.has_value());
    if (t) {
        CHECK(t->imu.size() == 4 && t->imu[3].timestamp == 1003000);
        CHECK(t->exposure.size() == 2 && t->exposure[1].timestamp == 1033667);
        CHECK(t->first_frame_timestamp == 1000300);
        CHECK(near(t->rolling_shutter_ms, 21.247, 1e-12));
        CHECK(t->has_imu_mount && t->imu_mount[0] == 0.5 && t->imu_mount[1] == 0.5 &&
              t->imu_mount[2] == -0.5 && t->imu_mount[3] == 0.5);
        CHECK(t->attitude.size() == 2);
        if (t->attitude.size() == 2) {
            const insv::AttitudeSample& a = t->attitude[0];
            CHECK(a.timestamp == 1000000 && a.q[0] == 1.0 && a.q[1] == 0.0 && a.q[3] == 0.0);
            CHECK(a.velocity_ned[0] == 1.5 && a.velocity_ned[1] == -2.0 &&
                  a.velocity_ned[2] == 0.25);
            const insv::AttitudeSample& b = t->attitude[1];
            CHECK(b.timestamp == 1020000 && near(b.q[0], c30, 1e-6) && near(b.q[2], s30, 1e-6));
        }
    }
    std::filesystem::remove(path);
}

// A drone's GPS without field 98 (the Antigravity A1): its fixes lined up
// with the video by matching their speeds against the flight controller's
// velocity, timed by field 24 on the IMU's clock. Not when the speed hardly
// changes, or when another shift fits as well.
void testGpsAlignedBySpeed()
{
    std::printf("  GPS lined up by the flight controller's speed\n");
    // Speeding up, a turn and braking: no two stretches alike.
    const auto speed = [](double t) {
        return t < 4.0 ? 2.0 * t : 8.0 + 3.0 * std::sin(0.4 * (t - 4.0)) * std::exp(-0.03 * t);
    };
    constexpr uint64_t kFirstFrameUs = 2000000, kFirstFrameUnixMs = 1793220455470ull;
    const auto clip = [&](const char* name, bool hover) {
        Bytes meta;
        meta.pbString(2, "Antigravity A1").pbString(54, kOneX2V3).pbVarint(24, kFirstFrameUs);
        Bytes attitude; // 50 Hz from just before the first frame, the speed north-east
        for (int k = -10; k < 1500; ++k) {
            const double t = k * 0.02, v = hover ? 0.1 : speed(t);
            attitude.u64(kFirstFrameUs + int64_t(k) * 20000).f32(0).f32(0).f32(0).f32(1);
            attitude.f32(float(v * 0.6)).f32(float(v * 0.8)).f32(0.3f);
        }
        Bytes gps; // a fix every 1.6 s from 1.13 s into the video
        for (int k = 0; k < 18; ++k) {
            const double t = 1.13 + k * 1.6;
            gps.raw(testtrailer::gpsRecord(kFirstFrameUnixMs + uint64_t(std::llround(t * 1000)),
                                           'A', 43.3, 'N', 73.6, 'W', hover ? 0.1 : speed(t),
                                           190.0, 105.0));
        }
        const std::string path = (std::filesystem::temp_directory_path() / name).string();
        std::ofstream f(path, std::ios::binary);
        const std::vector<uint8_t> video(64, 0x11);
        const std::vector<uint8_t> t = insvTrailer(
            {{0x101, meta.b}, {0x700, gps.b}, {0x2500, attitude.b}}, true, 256);
        f.write(reinterpret_cast<const char*>(video.data()), std::streamsize(video.size()));
        f.write(reinterpret_cast<const char*>(t.data()), std::streamsize(t.size()));
        return path;
    };
    const std::string flying = clip("insv_test_gps_speed.insv", false);
    const auto g = insv::ReadInsvGps(flying);
    CHECK(g && g->fixes.size() == 18 && g->aligned_by_speed);
    CHECK(g && std::llabs(g->first_frame_unix_ms - int64_t(kFirstFrameUnixMs)) <= 2);
    std::filesystem::remove(flying);
    const std::string hovering = clip("insv_test_gps_hover.insv", true);
    const auto h = insv::ReadInsvGps(hovering);
    CHECK(h && h->fixes.size() == 18 && h->first_frame_unix_ms == 0 && !h->aligned_by_speed);
    std::filesystem::remove(hovering);

    // A speed that repeats every 3.2 s fits two fixes on as well as on time.
    std::vector<insv::SpeedSample> reference, fixes;
    for (int k = 0; k < 1500; ++k)
        reference.push_back({k * 0.02, 6.0 + 3.0 * std::sin(2.0 * kPi * k * 0.02 / 3.2)});
    for (int k = 0; k < 15; ++k)
        fixes.push_back({k * 1.6, 6.0 + 3.0 * std::sin(2.0 * kPi * (k * 1.6 + 0.7) / 3.2)});
    CHECK(!insv::AlignGpsBySpeed(fixes, reference));
    // The same fixes against a speed that doesn't repeat: on time.
    reference.clear(), fixes.clear();
    for (int k = 0; k < 1500; ++k)
        reference.push_back({k * 0.02, speed(k * 0.02)});
    for (int k = 0; k < 15; ++k)
        fixes.push_back({k * 1.6, speed(k * 1.6 + 0.7)});
    const auto shift = insv::AlignGpsBySpeed(fixes, reference);
    CHECK(shift && near(*shift, 0.7, 0.002));
    CHECK(!insv::AlignGpsBySpeed({fixes.begin(), fixes.begin() + 4}, reference)); // too few
}

// A ride as the camera's IMU records it: a vehicle taking corners between
// straights while it speeds up and slows down, leaning into each turn as a motorcycle
// does, the camera on it turned 40 deg off its travel and tilted 8 deg.
// Samples in the sphere's axes (+X right, +Y up, -Z forward), 500 Hz.
namespace ride {
using M3 = std::array<std::array<double, 3>, 3>;
M3 mul(const M3& a, const M3& b)
{
    M3 c{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                c[i][j] += a[i][k] * b[k][j];
    return c;
}
M3 rotY(double a) { return {{{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}}}; }
M3 rotZ(double a) { return {{{std::cos(a), -std::sin(a), 0}, {std::sin(a), std::cos(a), 0}, {0, 0, 1}}}; }
M3 rotX(double a) { return {{{1, 0, 0}, {0, std::cos(a), -std::sin(a)}, {0, std::sin(a), std::cos(a)}}}; }

constexpr double kG = 9.80665;
struct Ride {
    double turnRate; // rad/s amplitude, 0: straight on
    double speed(double t) const { return 12.0 + 3.0 * std::sin(2 * kPi * t / 23.0); }
    double accel(double t) const { return 3.0 * 2 * kPi / 23.0 * std::cos(2 * kPi * t / 23.0); }
    // Positive turning right: from 10 s on, a corner every 10 s (4 s long,
    // left and right in turn) with straights between.
    double turn(double t) const
    {
        if (t < 10.0)
            return 0.0;
        const double u = std::fmod(t - 10.0, 10.0);
        const double side = std::fmod(std::floor((t - 10.0) / 10.0), 2.0) == 0.0 ? 1.0 : -1.0;
        return u < 4.0 ? side * turnRate * std::sin(kPi * u / 4.0) : 0.0;
    }
    // The heading the turns add up to: each corner turns by 8 rad/s * rate / pi.
    double heading(double t) const
    {
        if (t < 10.0)
            return 0.0;
        double psi = 0.0;
        const int corners = static_cast<int>(std::floor((t - 10.0) / 10.0));
        for (int c = 0; c < corners; ++c)
            psi += (c % 2 == 0 ? 1.0 : -1.0) * turnRate * 8.0 / kPi;
        const double u = std::min(std::fmod(t - 10.0, 10.0), 4.0);
        return psi + (corners % 2 == 0 ? 1.0 : -1.0) * turnRate * 4.0 / kPi * (1.0 - std::cos(kPi * u / 4.0));
    }
    double lean(double t) const { return std::atan(speed(t) * turn(t) / kG); } // positive right
    // Camera body -> world: the heading (turning right is clockwise from
    // above), the lean about the travel (-Z), then the mount.
    M3 orientation(double t) const
    {
        return mul(mul(rotY(-heading(t)), rotZ(-lean(t))), mul(rotY(40 * kDeg), rotX(8 * kDeg)));
    }
};

insv::ImuTrack record(const Ride& r, double seconds)
{
    insv::ImuTrack track;
    track.hasOffset = true;
    const double dt = 0.002, h = 1e-4;
    for (double t = 0.0; t < seconds; t += dt) {
        insv::ImuSample s;
        s.timestamp = static_cast<uint64_t>(std::llround(t * 1e6));
        const M3 q = r.orientation(t);
        // Specific force in g: the acceleration plus gravity's reaction (up).
        const double psi = r.heading(t);
        const double fwd[3] = {std::sin(psi), 0, -std::cos(psi)}, right[3] = {std::cos(psi), 0, std::sin(psi)};
        const double lat = r.speed(t) * r.turn(t);
        double world[3];
        for (int k = 0; k < 3; ++k)
            world[k] = (r.accel(t) * fwd[k] + lat * right[k]) / kG + (k == 1 ? 1.0 : 0.0);
        for (int i = 0; i < 3; ++i)
            s.accel[i] = q[0][i] * world[0] + q[1][i] * world[1] + q[2][i] * world[2];
        // Body rate from R(t-h)^T R(t+h) ~ I + 2h [w]x.
        const M3 a = r.orientation(t - h), b = r.orientation(t + h);
        M3 d{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    d[i][j] += a[k][i] * b[k][j];
        s.gyro[0] = (d[2][1] - d[1][2]) / (4 * h);
        s.gyro[1] = (d[0][2] - d[2][0]) / (4 * h);
        s.gyro[2] = (d[1][0] - d[0][1]) / (4 * h);
        track.samples.push_back(s);
    }
    return track;
}
} // namespace ride

// The lean and the acceleration along and across the travel, with the camera
// turned off the travel and tilted on the vehicle.
void testMotion()
{
    std::printf("  motion: lean and G about the travel, a turned and tilted mount\n");
    const ride::Ride weave{0.35};
    const insv::MotionTrack m = insv::BuildMotion(ride::record(weave, 90.0));
    CHECK(m.size() > 8900 && m.travelFit > 0.9);
    // Against the truth smoothed as the track is (0.25 s lean, 0.5 s G).
    std::vector<double> lean, lat, fwd;
    for (size_t i = 0; i < m.size(); ++i) {
        const double t = static_cast<double>(i) * m.stepSec;
        lean.push_back(weave.lean(t) / kDeg);
        lat.push_back(weave.speed(t) * weave.turn(t) / ride::kG);
        fwd.push_back(weave.accel(t) / ride::kG);
    }
    lean = insv::GaussianSmooth(lean, m.stepSec, 0.25);
    lat = insv::GaussianSmooth(lat, m.stepSec, 0.5);
    fwd = insv::GaussianSmooth(fwd, m.stepSec, 0.5);
    double leanErr = 0, latErr = 0, fwdErr = 0, maxLean = 0;
    for (size_t i = 500; i + 500 < m.size(); ++i) { // the smoothing's ends aside
        leanErr = std::max(leanErr, std::fabs(m.leanDeg[i] - lean[i]));
        latErr = std::max(latErr, std::fabs(m.lateralG[i] - lat[i]));
        fwdErr = std::max(fwdErr, std::fabs(m.forwardG[i] - fwd[i]));
        maxLean = std::max(maxLean, std::fabs(lean[i]));
    }
    std::printf("    lean to %.1f deg off by %.2f, lateral G by %.3f, forward G by %.3f\n", maxLean,
                leanErr, latErr, fwdErr);
    CHECK(maxLean > 25.0);
    CHECK(leanErr < 2.5 && latErr < 0.04 && fwdErr < 0.04);

    // Straight on: nothing to fit the travel to, so the camera's forward --
    // 40 deg off it, so the speeding up shows mostly along it.
    const ride::Ride straight{0.0};
    const insv::MotionTrack s = insv::BuildMotion(ride::record(straight, 60.0));
    CHECK(s.size() > 5900 && s.travelFit == 0.0);
    double sLean = 0, sFwd = 0, sRatio = 0;
    for (size_t i = 500; i + 500 < s.size(); ++i) {
        const double t = static_cast<double>(i) * s.stepSec;
        sLean = std::max(sLean, double(std::fabs(s.leanDeg[i])));
        sFwd = std::max(sFwd, std::fabs(s.forwardG[i] - std::cos(40 * kDeg) * straight.accel(t) / ride::kG));
        sRatio = std::max(sRatio, double(std::fabs(s.forwardG[i])));
    }
    // The level is gravity averaged over ~20 s, where 60 s of speeding up and
    // slowing down every 23 s leave part of a cycle: ~0.7 deg, 0.012 g.
    CHECK(sLean < 0.5 && sFwd < 0.015 && sRatio > 0.02);

    insv::ImuTrack noAccel = ride::record(weave, 5.0);
    for (insv::ImuSample& x : noAccel.samples)
        x.accel[0] = x.accel[1] = x.accel[2] = 0.0;
    CHECK(insv::BuildMotion(noAccel).empty());
    CHECK(insv::BuildMotion({}).empty());
}

// A GPS track timed by the camera's clock stamp, running behind the video by
// 2.1 s at the start and 1.5 s ten minutes later, lined up against the
// acceleration the IMU felt.
void testGpsAlignedByAcceleration()
{
    std::printf("  GPS lined up by the IMU's acceleration\n");
    const double step = 0.01, seconds = 600.0;
    // A speed that changes now and then, not in a repeating pattern.
    const auto speed = [](double t) {
        return 15.0 + 4.0 * std::sin(2 * kPi * t / 37.0) + 2.5 * std::sin(2 * kPi * t / 13.3 + 1.0) +
               1.5 * std::sin(2 * kPi * t / 5.9 + 2.0);
    };
    const auto behind = [&](double t) { return 2.1 - 0.6 * t / seconds; };
    std::vector<float> forwardG;
    for (double t = 0.0; t < seconds; t += step)
        forwardG.push_back(float((speed(t + 0.001) - speed(t - 0.001)) / 0.002 / ride::kG));
    insv::GpsTrack track;
    track.clockStamped = true;
    for (double t = 0.4; t < seconds; t += 1.0) // a fix a second, stamped late
        track.fixes.push_back({std::llround((t + behind(t)) * 1e3), 45.0, 7.0, speed(t), 90.0, 10.0});
    const auto fit = insv::AlignGpsByAcceleration(track, forwardG, step);
    CHECK(fit && fit->shifts.size() >= 4);
    if (fit) {
        double worst = 0.0;
        for (double t = 30.0; t < seconds - 30.0; t += 10.0)
            worst = std::max(worst, std::fabs(fit->shiftAt(t) - behind(t)));
        std::printf("    shift off by %.0f ms at most\n", worst * 1e3);
        CHECK(worst < 0.08);
        CHECK(fit->videoMs(2500) == 2500 - std::llround(fit->shiftAt(2.5) * 1e3));
    }

    // Standing still, or a steady speed: nothing to match.
    insv::GpsTrack steady = track;
    for (insv::GpsFix& f : steady.fixes)
        f.speedMps = 0.0;
    CHECK(!insv::AlignGpsByAcceleration(steady, forwardG, step));
    // A speed that has nothing to do with the IMU's.
    insv::GpsTrack other = track;
    for (size_t i = 0; i < other.fixes.size(); ++i)
        other.fixes[i].speedMps = speed(3.7 * double(i) + 100.0);
    CHECK(!insv::AlignGpsByAcceleration(other, forwardG, step));
    CHECK(!insv::AlignGpsByAcceleration({}, forwardG, step));
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
    testExtrinsicsOrder();
    testMeiProjection();
    testLensProfile();
    testLinearAlgebra();
    testSyntheticRender();
    testSeamDepthGeometry();
    testSeamPatchScore();
    testSeamDepthWindow();
    testSeamDepthUniqueness();
    testSeamWindowPathIndependence();
    testSeamAlignment();
    testSeamBlend();
    testSeamCutWindow();
    testSeamCutStats();
    testFitLensColour();
    testLensVeil();
    testLensColourClippedChannel();
    testLensColourWhereTheyMeet();
    testLensColourLocal();
    testSeamColourBlend();
    testSeamFlow();
    testBlurQuads();
    testTrailerThumbnail();
    testTrailerRecords();
    testGps();
    testOneX2Trailer();
    testA1Trailer();
    testCaptureTiming();
    testGpsAlignedBySpeed();
    testMotion();
    testGpsAlignedByAcceleration();
    testYcbcrRows();

    if (g_failures == 0)
        std::printf("insv_test: all checks passed\n");
    else
        std::printf("insv_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
