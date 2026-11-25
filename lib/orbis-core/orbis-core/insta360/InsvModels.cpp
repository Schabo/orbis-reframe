// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/insta360/InsvModels.h>

namespace orbiscore {
namespace {

// The X5's lens-profile table (field 145), as its videos carry it
// (InsvModel::lensProfiles): the image height in mm by the off-axis angle in
// degrees, and the field of view. The three guards carry the same curve.
const LensProfile kX5LensProfiles[] = {
    {"bare", {0, 0.03159, -8.4149999999999999e-05, 2.2010000000000002e-06, -1.284e-08, 0}, 200},
    {"ProtectorA",
     {0, 0.032190650500000001, -9.3724499999999993e-05, 2.5248e-06, -1.4999999999999999e-08, 0},
     195},
    {"ProtectorS",
     {0, 0.032190650500000001, -9.3724499999999993e-05, 2.5248e-06, -1.4999999999999999e-08, 0},
     195},
    {"ProtectorAS",
     {0, 0.032190650500000001, -9.3724499999999993e-05, 2.5248e-06, -1.4999999999999999e-08, 0},
     195},
    {"InvisibleDiveWater",
     {0.00023800000000000001, 0.031119999999999998, 7.306e-06, 4.4270000000000001e-07,
      -3.8920000000000002e-09, 0},
     190},
    {"InvisibleDiveAir",
     {0, 0.029933856599999999, 4.8687399999999997e-05, -6.7840000000000004e-07, 1.2e-09, 0},
     190},
};

const InsvModel kModels[] = {
    {"Insta360 X5", "Insta360", "z,-x,-y", "ProtectorS", "ProtectorA", 5312.0,
     {{3840, "8K"}, {2880, "5.7K"}, {1920, "4K"}, {0, nullptr}},
     {180.0, 0.0, 0.0}, 1.0, "_00_", nullptr, "LRV", "_01_", ".lrv", false,
     {1.0, 0.0, 0.0, 0.0}, 0.0, {}, "y,z,x", true, -0.5, 1799.0, "-z,-x,y", kX5LensProfiles,
     true},
    // IMU axes: the X5's "z,-x,-y", confirmed 2026-11-11 on an 8K 30
    // Standard clip with deliberate yaw/pitch/roll (docs/formats/insta360-x6.md):
    // residual_stab --diagnose horizon ~0.4 deg, correction/raw ratio 1.02 at
    // scale 1; residual pan lowest vs z,x,y and -x,y,-z on the same stretch.
    // File names and proxy layout: the X5's (one .insv + .lrv per take; no
    // X6 proxy checked yet). Timelapse orientation:
    // no X6 timelapse seen yet. Capture time: the X5's, unmeasured. Photos:
    // none seen yet.
    {"Insta360 X6", "Insta360", "z,-x,-y", nullptr, nullptr, 7680.0,
     {{3840, "8K"}, {3008, "6K"}, {1920, "4K"}, {0, nullptr}},
     {180.0, 0.0, 0.0}, 1.0, "_00_", nullptr, "LRV", "_01_", ".lrv", false,
     {1.0, 0.0, 0.0, 0.0}, 0.0, {}, nullptr, true, -0.5, 0.0, nullptr, {}, false},
    // A lens per file at 5.7K, proxy LRV_…_11_….insv, telemetry in ms, no
    // lens-guard profiles (docs/formats/insta360-one-x2.md §1, §6, §8). IMU
    // axes: "z,x,y", the X5's turned 180° about sphere X -- measured on both
    // sample takes by fitting the image rotation of four unstabilized views
    // against the gyro (doc §8). Capture time: one lag, the X5's from
    // before its exposure term was found; the ONE X2's own isn't measured
    // (its millisecond timestamps scatter the per-clip sync measurement by
    // about a millisecond, more than its short exposures move it).
    {"Insta360 ONE X2", "Insta360", "z,x,y", nullptr, nullptr, 2880.0,
     {{2880, "5.7K"}, {0, nullptr}, {0, nullptr}, {0, nullptr}},
     {180.0, 0.0, 0.0}, 1000.0, "_00_", "_10_", "LRV", "_11_", ".insv", true,
     {1.0, 0.0, 0.0, 0.0}, 0.0, {}, nullptr, false, -2.3, 0.0, nullptr, {}, false},
    // A 360 drone writing Insta360's .insv (docs/formats/antigravity-a1.md):
    // both lenses in one file, the proxy PRX_….prx with the clip's own name,
    // no lens-guard profiles, field 27 in the file. The 5.2K and 4K lens
    // sizes are from the spec sheet (no clips yet). IMU axes: "-x,y,-z" in
    // the sphere its mount gives, measured on two flight stretches of turns
    // by fitting the image rotation of four unstabilized views against the
    // gyro (1.3 deg from the free fit, gain ~1; raw +y is also where the
    // accelerometer puts "up" in a hover). Attitude: field 69 as on all
    // three sample clips; the camera sits pitched up 9.7 deg in the
    // airframe, fitted from the attitude's rotation against the gyro on all
    // three (9.5-9.9 deg; one unit measured). Lens correction: the median of
    // the per-clip seam fit (orbiscore::EstimateSeamAlignment) over 17 sample
    // clips in every mode, term by term: the lenses 0.14 deg apart at the
    // seam on average, against 0.42 with the file's calibration alone, and
    // the sharpest seam of the corrections tried. One unit measured, its
    // calibration the same in every clip (docs/formats/antigravity-a1.md).
    {"Antigravity A1", "Antigravity", "-x,y,-z", nullptr, nullptr, 5248.0,
     {{3840, "8K"}, {2624, "5.2K"}, {1920, "4K"}, {0, nullptr}},
     {-90.0, 90.0, 0.0}, 1.0, nullptr, nullptr, "PRX", nullptr, ".prx", true,
     {0.5, 0.5, -0.5, 0.5}, 9.7, {{0.006, 0.238, 0.468}, {-0.000749, 0.000016}, 0.998714}, nullptr,
     true, -0.5, 0.0, nullptr, {}, false},
};

} // namespace

std::span<const InsvModel> InsvModels() { return kModels; }

void ApplyInsvModelCalibration(const InsvModel& model, CameraCalibration& calib) {
    if (!model.radialModelsExchanged || calib.lensesExt.size() != 2)
        return;
    ExchangeRadialModels(calib.lensesExt[0], calib.lensesExt[1]);
}

const char* InsvModel::captureMode(int lensWidth) const {
    for (const CaptureMode& m : captureModes)
        if (m.name && m.lensWidth == lensWidth)
            return m.name;
    return "";
}

const InsvModel& FindInsvModel(const std::string& model, bool* known) {
    for (const InsvModel& m : kModels) {
        if (model == m.name) {
            if (known)
                *known = true;
            return m;
        }
    }
    if (known)
        *known = false;
    return kModels[0];
}

} // namespace orbiscore
