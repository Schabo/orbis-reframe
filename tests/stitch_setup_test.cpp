// stitch::DualFisheyeStitchSetup and the camera model's turn of lens 1
// (insv::InsvModel::lens1TurnDeg): which cameras carry one, that the set-up
// applies it only when asked (the new stitcher) and only over the file's
// calibration, not a chart calibration, and that the stitch turns lens 1 by
// it as by the seam correction's turn. No GPU, no clip. Exit code is the
// failure count.

#include "LensCalibration.h"
#include "NativeStitchSource.h"

#include <ClipMetadata.h>
#include <FisheyeProjection.h>
#include <InsvModels.h>

#include <cmath>
#include <cstdio>
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

// A lens of the A1's (docs/formats/antigravity-a1.md): MEI, pitch -90.
insv::LensCalibExt a1Lens(int i)
{
    insv::LensCalibExt e;
    e.xi = 1.50377;
    e.fx = i ? 3754.78 : 3742.54;
    e.fy = i ? 3755.14 : 3741.81;
    e.cx = i ? 7875.89 : 2620.87;
    e.cy = i ? 2606.19 : 2617.55;
    e.yaw = i ? -179.449 : -177.49;
    e.pitch = i ? -89.922 : -89.95;
    e.roll = i ? 89.441 : 90.397;
    e.tz = i ? -0.071126 : 0.0;
    e.k[0] = -0.0563;
    e.k[1] = 0.2038;
    e.k[2] = -0.5345;
    e.w = 10496;
    e.h = 5248;
    e.focalRefW = 5248;
    return e;
}

camera::ClipMetadata a1Clip()
{
    camera::ClipMetadata c;
    c.model = "Antigravity A1";
    c.projection = camera::Projection::DualFisheye;
    c.lenses.resize(2);
    for (int i = 0; i < 2; ++i)
        c.lenses[i].calibration = a1Lens(i);
    c.calibrated = true;
    const insv::InsvModel& m = insv::FindInsvModel(c.model);
    for (int i = 0; i < 3; ++i)
        c.lens1TurnDeg[i] = m.lens1TurnDeg[i];
    c.mountYawDeg = m.mountDeg[0];
    c.mountPitchDeg = m.mountDeg[1];
    c.mountRollDeg = m.mountDeg[2];
    return c;
}

bool sameTurn(const insv::StitchParams& p, const double t[3])
{
    return p.lens1TurnDeg[0] == t[0] && p.lens1TurnDeg[1] == t[1] && p.lens1TurnDeg[2] == t[2];
}

void testModels()
{
    // Insta360's files line the lenses up as they are; the A1's need a turn.
    for (const insv::InsvModel& m : insv::InsvModels()) {
        const bool zero = m.lens1TurnDeg[0] == 0 && m.lens1TurnDeg[1] == 0 && m.lens1TurnDeg[2] == 0;
        if (std::string(m.maker) == "Insta360")
            CHECK(zero);
        else
            CHECK(!zero);
    }
}

void testSetup()
{
    const camera::ClipMetadata clip = a1Clip();
    const double none[3] = {0, 0, 0};
    std::string err;
    {
        insv::InsvCalibration cal;
        insv::StitchParams p;
        CHECK(stitch::DualFisheyeStitchSetup(clip, stitch::PreviewStitchConfig::LensGuard::None,
                                             nullptr, true, cal, p, err));
        CHECK(sameTurn(p, clip.lens1TurnDeg));
    }
    {
        // The old stitcher and its per-clip fit: the file's as it is.
        insv::InsvCalibration cal;
        insv::StitchParams p;
        CHECK(stitch::DualFisheyeStitchSetup(clip, stitch::PreviewStitchConfig::LensGuard::None,
                                             nullptr, false, cal, p, err));
        CHECK(sameTurn(p, none));
    }
    {
        // A chart calibration replaces the file's, and the turn with it.
        stitch::LensCalibration chart;
        for (int i = 0; i < 2; ++i) {
            chart.fileLenses[i] = clip.lenses[i].calibration;
            chart.lenses[i] = clip.lenses[i].calibration;
        }
        chart.lenses[1].pitch += 0.2;
        insv::InsvCalibration cal;
        insv::StitchParams p;
        CHECK(stitch::DualFisheyeStitchSetup(clip, stitch::PreviewStitchConfig::LensGuard::None,
                                             &chart, true, cal, p, err));
        CHECK(sameTurn(p, none));
        CHECK(cal.lensesExt[1].pitch == chart.lenses[1].pitch);
    }
    {
        // One for another camera's file: the file's, with the turn.
        stitch::LensCalibration other;
        for (int i = 0; i < 2; ++i) {
            other.fileLenses[i] = clip.lenses[i].calibration;
            other.lenses[i] = clip.lenses[i].calibration;
        }
        other.fileLenses[0].cx += 1.0;
        insv::InsvCalibration cal;
        insv::StitchParams p;
        CHECK(stitch::DualFisheyeStitchSetup(clip, stitch::PreviewStitchConfig::LensGuard::None,
                                             &other, true, cal, p, err));
        CHECK(sameTurn(p, clip.lens1TurnDeg));
    }
}

void testStitchTurn()
{
    // The model's turn moves lens 1 exactly as the same seam-correction turn,
    // and leaves lens 0 where it was.
    const camera::ClipMetadata clip = a1Clip();
    insv::InsvCalibration cal;
    insv::StitchParams base;
    std::string err;
    CHECK(stitch::DualFisheyeStitchSetup(clip, stitch::PreviewStitchConfig::LensGuard::None,
                                         nullptr, false, cal, base, err));
    insv::StitchParams turned = base, viaSeam = base;
    for (int i = 0; i < 3; ++i) {
        turned.lens1TurnDeg[i] = clip.lens1TurnDeg[i];
        viaSeam.seam.lens1RotDeg[i] = clip.lens1TurnDeg[i];
    }
    insv::FisheyeStitcher a, b, c;
    CHECK(a.configure(cal, 3840, 3840, base));
    CHECK(b.configure(cal, 3840, 3840, turned));
    CHECK(c.configure(cal, 3840, 3840, viaSeam));
    double lens0 = 0, lens1 = 0, seamDiff = 0;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            lens0 = std::max(lens0, std::fabs(a.lensProjection(0).worldToLens.m[r][k] -
                                              b.lensProjection(0).worldToLens.m[r][k]));
            lens1 = std::max(lens1, std::fabs(a.lensProjection(1).worldToLens.m[r][k] -
                                              b.lensProjection(1).worldToLens.m[r][k]));
            seamDiff = std::max(seamDiff, std::fabs(b.lensProjection(1).worldToLens.m[r][k] -
                                                    c.lensProjection(1).worldToLens.m[r][k]));
        }
    CHECK(lens0 == 0.0);
    CHECK(lens1 > 1e-3); // ~0.5 deg
    CHECK(seamDiff == 0.0);
}

} // namespace

int main()
{
    testModels();
    testSetup();
    testStitchTurn();
    if (g_failures == 0)
        std::printf("stitch_setup_test: all checks passed\n");
    return g_failures;
}
