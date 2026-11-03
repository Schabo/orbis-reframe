// stitch::DualFisheyeStitchSetup and the camera model's correction to its
// files' lens calibration (insv::InsvModel::lensCorrection): which cameras
// carry one, that the set-up applies it only when asked (the new stitcher)
// and only over the file's calibration, not a chart calibration, and that
// the stitch applies it as it does the seam correction, the two composing.
// No GPU, no clip. Exit code is the failure count.

#include "LensCalibration.h"
#include "NativeStitchSource.h"

#include <ClipMetadata.h>
#include <FisheyeProjection.h>
#include <InsvModels.h>

#include <algorithm>
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
    c.lensCorrection = m.lensCorrection;
    c.mountYawDeg = m.mountDeg[0];
    c.mountPitchDeg = m.mountDeg[1];
    c.mountRollDeg = m.mountDeg[2];
    return c;
}


void testModels()
{
    // Insta360's files line the lenses up as they are; the A1's need a
    // correction.
    for (const insv::InsvModel& m : insv::InsvModels()) {
        const bool none = m.lensCorrection == insv::SeamCorrection{};
        if (std::string(m.maker) == "Insta360")
            CHECK(none);
        else
            CHECK(!none);
    }
}

void testSetup()
{
    const camera::ClipMetadata clip = a1Clip();
    const insv::SeamCorrection none;
    std::string err;
    {
        insv::InsvCalibration cal;
        insv::StitchParams p;
        CHECK(stitch::DualFisheyeStitchSetup(clip, stitch::PreviewStitchConfig::LensGuard::None,
                                             nullptr, true, cal, p, err));
        CHECK(p.modelCorrection == clip.lensCorrection);
    }
    {
        // The old stitcher and its per-clip fit: the file's as it is.
        insv::InsvCalibration cal;
        insv::StitchParams p;
        CHECK(stitch::DualFisheyeStitchSetup(clip, stitch::PreviewStitchConfig::LensGuard::None,
                                             nullptr, false, cal, p, err));
        CHECK(p.modelCorrection == none);
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
        CHECK(p.modelCorrection == none);
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
        CHECK(p.modelCorrection == clip.lensCorrection);
    }
}

// The largest difference between two stitchers' lens models.
double lensDiff(const insv::FisheyeStitcher& a, const insv::FisheyeStitcher& b, int lens)
{
    const insv::LensProjection& x = a.lensProjection(lens);
    const insv::LensProjection& y = b.lensProjection(lens);
    double d = std::max({std::fabs(x.fx - y.fx), std::fabs(x.fy - y.fy), std::fabs(x.cx - y.cx),
                         std::fabs(x.cy - y.cy)});
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            d = std::max(d, std::fabs(x.worldToLens.m[r][k] - y.worldToLens.m[r][k]));
    return d;
}

void testStitchCorrection()
{
    // The model's correction moves the lenses exactly as the same seam
    // correction would, and the two compose: turns and centres add, scales
    // multiply.
    const camera::ClipMetadata clip = a1Clip();
    insv::InsvCalibration cal;
    insv::StitchParams base;
    std::string err;
    CHECK(stitch::DualFisheyeStitchSetup(clip, stitch::PreviewStitchConfig::LensGuard::None,
                                         nullptr, false, cal, base, err));
    const insv::SeamCorrection& m = clip.lensCorrection;
    insv::StitchParams model = base, viaSeam = base;
    model.modelCorrection = m;
    viaSeam.seam = m;
    insv::FisheyeStitcher a, b, c;
    CHECK(a.configure(cal, 3840, 3840, base));
    CHECK(b.configure(cal, 3840, 3840, model));
    CHECK(c.configure(cal, 3840, 3840, viaSeam));
    CHECK(lensDiff(a, b, 1) > 1e-3); // lens 1 moves (~0.5 deg)
    CHECK(lensDiff(b, c, 0) == 0.0 && lensDiff(b, c, 1) == 0.0);

    insv::SeamCorrection s;
    s.lens1RotDeg[0] = 0.1;
    s.lens1RotDeg[2] = -0.2;
    s.lens1Centre[1] = 0.0003;
    s.radialScale = 1.002;
    insv::StitchParams both = base, summed = base;
    both.modelCorrection = m;
    both.seam = s;
    for (int i = 0; i < 3; ++i)
        summed.seam.lens1RotDeg[i] = m.lens1RotDeg[i] + s.lens1RotDeg[i];
    for (int i = 0; i < 2; ++i)
        summed.seam.lens1Centre[i] = m.lens1Centre[i] + s.lens1Centre[i];
    summed.seam.radialScale = m.radialScale * s.radialScale;
    insv::FisheyeStitcher d, e;
    CHECK(d.configure(cal, 3840, 3840, both));
    CHECK(e.configure(cal, 3840, 3840, summed));
    CHECK(lensDiff(d, e, 0) < 1e-9 && lensDiff(d, e, 1) < 1e-9);
}

} // namespace

int main()
{
    testModels();
    testSetup();
    testStitchCorrection();
    if (g_failures == 0)
        std::printf("stitch_setup_test: all checks passed\n");
    return g_failures;
}
