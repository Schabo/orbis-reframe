// src/calib on synthetic data, where the answer is known: a "true" camera
// that differs from its file's calibration (each lens's scale, centre and
// distortion, lens 1's rotation), the chart seen by it from many poses. The
// fit, started from the file's, must find the true lenses; the detector must
// find the chart's corners in a fisheye picture of it. No clip, no GPU. Exit
// code is the failure count.

#include "ChartBoard.h"
#include "ChartDetect.h"
#include "LensFit.h"

#include <ClipMetadata.h>
#include <FisheyeProjection.h>
#include <LensCalibrationJson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                          \
            ++g_failures;                                                                         \
        }                                                                                         \
    } while (0)

constexpr double kDeg = std::numbers::pi / 180;
constexpr double kSquareMm = 42.0;

// An X5's file calibration (both lenses; frame 10752 x 5376, focal window
// 5312), as its 56-token string has it.
insv::LensCalibExt X5Lens(int i)
{
    insv::LensCalibExt e;
    e.xi = 2;
    e.w = 10752;
    e.h = 5376;
    e.focalRefW = 5312;
    e.type = 113;
    if (i == 0) {
        e.fx = 4294.03, e.fy = 4293.86, e.cx = 2703.31, e.cy = 2684.03;
        e.yaw = 0.241, e.pitch = 0.068, e.roll = 89.993;
        const double k[4] = {0.22969893, 1.60199785, -1.24233508, -3.16975212};
        const double s[4] = {-0.00395632, -0.00326463, 0.00195414, 0.00044626};
        std::copy(k, k + 4, e.k);
        std::copy(s, s + 4, e.s);
        e.p[0] = -0.00144594, e.p[1] = 0.00040147;
    } else {
        e.fx = 4277.66, e.fy = 4277.27, e.cx = 8073.21, e.cy = 2692.04;
        e.yaw = -0.088, e.pitch = -0.141, e.roll = 88.58;
        e.tx = 0.000774, e.ty = 0.000174, e.tz = -0.031796;
        const double k[4] = {0.24695949, 1.25187707, 0.2654447, -5.21495152};
        const double s[4] = {0.00577204, 0.01037207, -0.00314844, -0.00378038};
        std::copy(k, k + 4, e.k);
        std::copy(s, s + 4, e.s);
        e.p[0] = -0.00144929, e.p[1] = -0.00059055;
    }
    return e;
}

camera::ClipMetadata Clip(int lensSize)
{
    camera::ClipMetadata c;
    c.serial = "TEST0001";
    c.calibrated = true;
    c.lensWidth = c.lensHeight = lensSize;
    c.fps = 30;
    c.lenses.resize(2);
    for (int i = 0; i < 2; ++i)
        c.lenses[i].calibration = X5Lens(i);
    return c;
}

// The true camera: the file's, changed as the chart finds real ones are.
insv::InsvCalibration TrueCalibration(const camera::ClipMetadata& clip)
{
    insv::InsvCalibration cal;
    cal.lensesExt = {clip.lenses[0].calibration, clip.lenses[1].calibration};
    insv::LensCalibExt &a = cal.lensesExt[0], &b = cal.lensesExt[1];
    a.fx *= 0.995, a.fy *= 0.995, a.cx += 3, a.cy -= 2, a.k[0] += 0.004;
    b.fx *= 1.005, b.fy *= 1.004, b.cx -= 2, b.cy += 4, b.k[1] -= 0.03;
    b.yaw += 0.2, b.pitch -= 0.1;
    return cal;
}

insv::Vec3 Axis(const insv::FisheyeStitcher& st, int lens)
{
    const insv::Mat3& w = st.lensProjection(lens).worldToLens;
    return {w.m[2][0], w.m[2][1], w.m[2][2]};
}

insv::Vec3 Add(insv::Vec3 a, insv::Vec3 b, double s = 1) { return {a.x + s * b.x, a.y + s * b.y, a.z + s * b.z}; }
insv::Vec3 Cross(insv::Vec3 a, insv::Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// A board pose: its centre `dist` mm along `dir`, facing the camera with a
// tilt, turned in its plane. Corner id -> world point (mm, lens 0 at 0).
struct Board {
    insv::Vec3 centre, ex, ey; // ex, ey: the board's axes, unit
    insv::Vec3 corner(int id) const
    {
        double x, y;
        calib::ChartCornerPosition(id, x, y);
        // About the board's middle (6.5, 3.5 squares).
        return Add(Add(centre, ex, (x - 0.5 * calib::kChartSquaresX) * kSquareMm), ey,
                   (y - 0.5 * calib::kChartSquaresY) * kSquareMm);
    }
};

Board MakeBoard(insv::Vec3 dir, double dist, double tiltDeg, double spinDeg, double tiltAzDeg)
{
    dir = insv::normalize(dir);
    insv::Vec3 up{0, 1, 0};
    if (std::fabs(dir.y) > 0.9)
        up = {1, 0, 0};
    insv::Vec3 u = insv::normalize(Cross(up, dir)), v = Cross(dir, u);
    // Spin in the plane, then tilt the normal away from the camera axis.
    const double s = spinDeg * kDeg, t = tiltDeg * kDeg, a = tiltAzDeg * kDeg;
    insv::Vec3 ex = Add(insv::Vec3{u.x * std::cos(s), u.y * std::cos(s), u.z * std::cos(s)}, v, std::sin(s));
    insv::Vec3 ey = Cross(dir, ex);
    const insv::Vec3 tiltAxis = Add(insv::Vec3{ex.x * std::cos(a), ex.y * std::cos(a), ex.z * std::cos(a)},
                                    ey, std::sin(a));
    auto rot = [&](insv::Vec3 p) { // Rodrigues about tiltAxis by t
        const insv::Vec3 k = tiltAxis, kxp = Cross(k, p);
        const double kp = k.x * p.x + k.y * p.y + k.z * p.z;
        return insv::Vec3{p.x * std::cos(t) + kxp.x * std::sin(t) + k.x * kp * (1 - std::cos(t)),
                          p.y * std::cos(t) + kxp.y * std::sin(t) + k.y * kp * (1 - std::cos(t)),
                          p.z * std::cos(t) + kxp.z * std::sin(t) + k.z * kp * (1 - std::cos(t))};
    };
    return {{dir.x * dist, dir.y * dist, dir.z * dist}, rot(ex), rot(ey)};
}

void testFit()
{
    const camera::ClipMetadata clip = Clip(3840);
    const insv::InsvCalibration truth = TrueCalibration(clip);
    insv::FisheyeStitcher st;
    insv::StitchParams params;
    params.depthSeam = true; // project out to the circle's edge (100 deg)
    CHECK(st.configure(truth, 3840, 3840, params));
    // Lens 1's centre: the file's spacing behind lens 0 along its axis.
    const insv::Vec3 a0 = Axis(st, 0), a1 = Axis(st, 1);
    const double baseMm = st.baselineM() * 1000;
    const insv::Vec3 c1{-a0.x * baseMm, -a0.y * baseMm, -a0.z * baseMm};

    std::mt19937 rng(11);
    std::uniform_real_distribution<double> uni(0, 1);
    std::normal_distribution<double> noise(0, 0.15);
    calib::ChartObservations obs;
    obs.lensWidth = obs.lensHeight = 3840;
    obs.fps = 30;
    int frame = 0;
    for (int k = 0; k < 520; ++k) {
        // Around one lens's axis, out to 98 deg, the far ones seen by both.
        const insv::Vec3 axis = k % 2 ? a1 : a0;
        insv::Vec3 up{0, 1, 0};
        const insv::Vec3 u = insv::normalize(Cross(up, axis)), v = Cross(axis, u);
        const double th = std::sqrt(uni(rng)) * 98 * kDeg, ph = uni(rng) * 2 * std::numbers::pi;
        const insv::Vec3 dir = Add(Add(insv::Vec3{axis.x * std::cos(th), axis.y * std::cos(th), axis.z * std::cos(th)},
                                       u, std::sin(th) * std::cos(ph)),
                                   v, std::sin(th) * std::sin(ph));
        const Board b = MakeBoard(dir, 550 + 350 * uni(rng), 35 * uni(rng), 360 * uni(rng),
                                  360 * uni(rng));
        calib::ChartView view[2];
        for (int l = 0; l < 2; ++l) {
            for (int id = 0; id < calib::kChartCorners; ++id) {
                insv::Vec3 p = b.corner(id);
                if (l == 1)
                    p = Add(p, c1, -1);
                double px, py, theta;
                if (!st.project(l, p, px, py, theta) || theta > 98 * kDeg)
                    continue;
                if (px < 0 || py < 0 || px >= 3840 || py >= 3840)
                    continue;
                view[l].corners.push_back({id, float(px + noise(rng)), float(py + noise(rng))});
            }
        }
        // Each pose twice, three frames apart: the board holds still.
        for (int rep = 0; rep < 2; ++rep, frame += 3)
            for (int l = 0; l < 2; ++l)
                if (view[l].corners.size() >= 12) {
                    calib::ChartView v2 = view[l];
                    v2.frame = frame;
                    obs.lens[l].push_back(v2);
                }
        frame += 3;
    }

    calib::LensFitOptions opt;
    opt.squareMm = kSquareMm;
    const calib::LensFitResult r = calib::FitLensCalibration(clip, obs, opt);
    CHECK(r.ok);
    if (!r.ok) {
        std::printf("  fit: %s\n", r.message.c_str());
        return;
    }
    std::printf("  frames %d / %d (both %d), rms %.3f / %.3f px (file %.3f / %.3f), transfer %.4f "
                "deg (file %.3f), coverage %.0f / %.0f deg\n",
                r.framesUsed[0], r.framesUsed[1], r.framesBoth, r.calibration.rmsPx[0],
                r.calibration.rmsPx[1], r.fileRmsPx[0], r.fileRmsPx[1], r.calibration.transferDeg,
                r.fileTransferDeg, r.calibration.coverageDeg[0], r.calibration.coverageDeg[1]);
    CHECK(calib::CheckLensFit(r).empty());
    // The file's model fits clearly worse (its poses take up some of it).
    CHECK(r.fileRmsPx[0] > 0.25 && r.fileRmsPx[1] > 0.25);
    CHECK(r.fileTransferDeg > 0.1 && r.calibration.transferDeg < 0.02);

    // The fitted calibration, stitched: where it puts each direction against
    // where the true lens has it.
    insv::InsvCalibration fitted;
    fitted.lensesExt = {clip.lenses[0].calibration, clip.lenses[1].calibration};
    CHECK(r.calibration.matchesFile(fitted.lensesExt[0], fitted.lensesExt[1]));
    stitch::ApplyLensCalibration(r.calibration, fitted.lensesExt[0], fitted.lensesExt[1]);
    insv::FisheyeStitcher sf;
    CHECK(sf.configure(fitted, 3840, 3840, params));
    for (int l = 0; l < 2; ++l) {
        double worst = 0;
        const insv::Vec3 axis = l ? a1 : a0;
        const insv::Vec3 u = insv::normalize(Cross(insv::Vec3{0, 1, 0}, axis)), v = Cross(axis, u);
        for (int th = 0; th <= 95; th += 5)
            for (int ph = 0; ph < 360; ph += 10) {
                const insv::Vec3 d =
                    Add(Add(insv::Vec3{axis.x * std::cos(th * kDeg), axis.y * std::cos(th * kDeg),
                                       axis.z * std::cos(th * kDeg)},
                            u, std::sin(th * kDeg) * std::cos(ph * kDeg)),
                        v, std::sin(th * kDeg) * std::sin(ph * kDeg));
                double x0, y0, x1, y1, t0, t1;
                if (!st.project(l, d, x0, y0, t0) || !sf.project(l, d, x1, y1, t1))
                    continue;
                worst = std::max(worst, std::hypot(x1 - x0, y1 - y0));
            }
        // ~19 px a degree: 0.3 px is 0.015 deg, at the rim where the corners
        // are fewest and the noise (0.15 px) counts most.
        std::printf("  lens %d: worst %.3f px from the truth to 95 deg\n", l, worst);
        CHECK(worst < 0.3);
    }

    // Kept and read back, it is the same calibration.
    stitch::LensCalibration c = r.calibration;
    c.serial = clip.serial;
    c.lensGuard = 2;
    c.created = "2026-11-02T12:00:00Z";
    const auto back = lenscal::FromJson(nlohmann::json::parse(lenscal::ToJson(c).dump()));
    CHECK(back && *back == c);
}

// The chart in a fisheye picture: rendered through the stitch's lens model
// (each pixel's ray, found by Newton through FisheyeStitcher::project, onto
// the board's plane), then found.
void testDetect()
{
    const int S = 1920;
    const camera::ClipMetadata clip = Clip(S);
    insv::InsvCalibration cal;
    cal.lensesExt = {clip.lenses[0].calibration, clip.lenses[1].calibration};
    insv::FisheyeStitcher st;
    insv::StitchParams params;
    params.depthSeam = true;
    CHECK(st.configure(cal, S, S, params));
    const insv::Vec3 a0 = Axis(st, 0);
    // 50 deg off the axis, 0.7 m away, tilted.
    const insv::Vec3 u = insv::normalize(Cross(insv::Vec3{0, 1, 0}, a0));
    const insv::Vec3 dir = Add(insv::Vec3{a0.x * std::cos(50 * kDeg), a0.y * std::cos(50 * kDeg),
                                          a0.z * std::cos(50 * kDeg)},
                               u, std::sin(50 * kDeg));
    const Board b = MakeBoard(dir, 700, 20, 15, 40);
    const calib::ChartImage chart = calib::MakeChartImage(13 * 80 + 160, 7 * 80 + 160);
    const int bx0 = (chart.width - 13 * chart.squarePx) / 2, by0 = (chart.height - 7 * chart.squarePx) / 2;

    // The board's corners' pixels, and a box around them to render.
    std::vector<calib::ChartCorner> truth;
    double minX = S, minY = S, maxX = 0, maxY = 0;
    for (int id = 0; id < calib::kChartCorners; ++id) {
        double x, y, th;
        CHECK(st.project(0, b.corner(id), x, y, th));
        truth.push_back({id, float(x), float(y)});
        minX = std::min(minX, x), maxX = std::max(maxX, x);
        minY = std::min(minY, y), maxY = std::max(maxY, y);
    }
    const int pad = 120;
    const int x0 = std::max(0, int(minX) - pad), x1 = std::min(S, int(maxX) + pad);
    const int y0 = std::max(0, int(minY) - pad), y1 = std::min(S, int(maxY) + pad);
    std::vector<uint8_t> img(size_t(S) * S, 128);
    const insv::Vec3 n = Cross(b.ex, b.ey);
    insv::Vec3 d = insv::normalize(b.centre);
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            // Newton: the direction d whose projection is the pixel centre (px, py).
            const insv::Vec3 e1 = insv::normalize(Cross(n, d)), e2 = Cross(d, e1);
            for (int it = 0; it < 8; ++it) {
                double x, y, th, xa, ya, xb, yb;
                st.project(0, d, x, y, th);
                const double h = 1e-5;
                st.project(0, Add(d, e1, h), xa, ya, th);
                st.project(0, Add(d, e2, h), xb, yb, th);
                const double j00 = (xa - x) / h, j10 = (ya - y) / h, j01 = (xb - x) / h,
                             j11 = (yb - y) / h, det = j00 * j11 - j01 * j10;
                // project()'s pixel frame: the centre of pixel (px, py) is at
                // (px, py). ChArUco's absolute origin differs by ~½ px between
                // OpenCV 4 and 5; the check below allows that global shift.
                const double ex = px - x, ey = py - y;
                const double s1 = (j11 * ex - j01 * ey) / det, s2 = (-j10 * ex + j00 * ey) / det;
                d = insv::normalize(Add(Add(d, e1, s1), e2, s2));
                if (std::fabs(ex) + std::fabs(ey) < 1e-4)
                    break;
            }
            // Onto the board's plane, in squares from its top-left.
            const double dn = d.x * n.x + d.y * n.y + d.z * n.z;
            const double tt = (b.centre.x * n.x + b.centre.y * n.y + b.centre.z * n.z) / dn;
            const insv::Vec3 p = Add(insv::Vec3{d.x * tt, d.y * tt, d.z * tt}, b.centre, -1);
            const double bxs = (p.x * b.ex.x + p.y * b.ex.y + p.z * b.ex.z) / kSquareMm + 6.5;
            const double bys = (p.x * b.ey.x + p.y * b.ey.y + p.z * b.ey.z) / kSquareMm + 3.5;
            const double cx = bx0 + bxs * chart.squarePx, cy = by0 + bys * chart.squarePx;
            const int ix = std::clamp(int(cx), 0, chart.width - 1), iy = std::clamp(int(cy), 0, chart.height - 1);
            img[size_t(py) * S + px] = chart.grey[size_t(iy) * chart.width + ix];
        }
    const std::vector<calib::ChartCorner> found = calib::DetectChartInPicture(img.data(), S, S);
    CHECK(found.size() >= 60);
    double meanDu = 0, meanDv = 0;
    for (const calib::ChartCorner& f : found) {
        meanDu += f.u - truth[f.id].u;
        meanDv += f.v - truth[f.id].v;
    }
    meanDu /= double(found.size());
    meanDv /= double(found.size());
    std::vector<double> err;
    err.reserve(found.size());
    for (const calib::ChartCorner& f : found)
        err.push_back(std::hypot(f.u - truth[f.id].u - meanDu, f.v - truth[f.id].v - meanDv));
    std::sort(err.begin(), err.end());
    const double bias = std::hypot(meanDu, meanDv);
    std::printf("  detected %zu of %d corners, bias %.3f px (du %.3f dv %.3f), "
                "scatter median %.3f worst %.3f\n",
                found.size(), calib::kChartCorners, bias, meanDu, meanDv,
                err.empty() ? -1.0 : err[err.size() / 2], err.empty() ? -1.0 : err.back());
    // Bias: OpenCV 4 vs 5 ChArUco origins differ by about half a pixel against
    // project(); anything much larger is a real shift. Scatter: geometry.
    CHECK(bias < 0.75);
    CHECK(!err.empty() && err[err.size() / 2] < 0.3 && err.back() < 1.0);
}

void testChartImage()
{
    const calib::ChartImage img = calib::MakeChartImage(2560, 1440);
    CHECK(img.squarePx == 180); // 2560 / 14, 1440 / 8
    // Found in its own picture, all of it.
    CHECK(calib::DetectChartInPicture(img.grey.data(), img.width, img.height).size() ==
          size_t(calib::kChartCorners));
}

} // namespace

int main()
{
    testChartImage();
    testDetect();
    testFit();
    if (g_failures == 0)
        std::printf("lens_calib_test: all ok\n");
    return g_failures;
}
