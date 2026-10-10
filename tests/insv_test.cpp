// Standalone checks for the cleanroom stitch core (src/insv/): calibration
// string parsing, lens profiles, the linear-algebra helpers, and
// FisheyeStitcher geometry. All synthetic -- no .insv file, no FFmpeg, no Qt --
// so it links only the dependency-free `insv` lib and runs anywhere. Exit code
// is the failure count. Mirrors tests/core_model_test.cpp's harness.

#include "BlurQuads.h"
#include "Calibration.h"
#include "FisheyeProjection.h"
#include "Image.h"
#include "SeamDepth.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

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

void testMeiProjection()
{
    insv::InsvCalibration c;
    CHECK(insv::ParseExtendedCalibrationString(kX5Extended, c));

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

    insv::StitchParams bare;
    CHECK(insv::ApplyLensProfile(bare, notes, ""));
    CHECK(!bare.accessory && near(bare.fovDeg, 200, 1e-9));

    insv::StitchParams guard;
    CHECK(insv::ApplyLensProfile(guard, notes, "ProtectorA"));
    CHECK(guard.accessory && near(guard.fovDeg, 195, 1e-9));

    insv::StitchParams missing;
    CHECK(!insv::ApplyLensProfile(missing, notes, "InvisibleDiveAir"));
    CHECK(!missing.accessory);

    insv::InsvCalibration c;
    CHECK(insv::ParseExtendedCalibrationString(kX5Extended, c));
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
    insv::InsvCalibration c;
    CHECK(insv::ParseExtendedCalibrationString(kX5Extended, c));
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
    insv::InsvCalibration c;
    CHECK(insv::ParseExtendedCalibrationString(kX5Extended, c));
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

int main()
{
    testExtendedCalibration();
    testMeiProjection();
    testLensProfile();
    testLinearAlgebra();
    testSyntheticRender();
    testSeamDepthGeometry();
    testSeamDepthTracker();
    testBlurQuads();

    if (g_failures == 0)
        std::printf("insv_test: all checks passed\n");
    else
        std::printf("insv_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
