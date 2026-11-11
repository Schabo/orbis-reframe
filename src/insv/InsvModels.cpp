// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvModels.h"

namespace insv {
namespace {

const InsvModel kModels[] = {
    {"Insta360 X5", "Insta360", "z,-x,-y", "ProtectorS", "ProtectorA", 5312.0,
     {{3840, "8K"}, {2880, "5.7K"}, {1920, "4K"}, {0, nullptr}},
     {180.0, 0.0, 0.0}, 1.0, "_00_", nullptr, "LRV", "_01_", ".lrv",
     {1.0, 0.0, 0.0, 0.0}, 0.0, {}, "y,z,x", true, -0.5},
    // IMU axes: the X5's "z,-x,-y", confirmed 2026-11-11 on an 8K 30
    // Standard clip with deliberate yaw/pitch/roll (docs/formats/insta360-x6.md):
    // residual_stab --diagnose horizon ~0.4 deg, correction/raw ratio 1.02 at
    // scale 1; residual pan lowest vs z,x,y and -x,y,-z on the same stretch.
    // File names: the X5's (one .insv + .lrv per take). Timelapse orientation:
    // no X6 timelapse seen yet. Capture time: the X5's, unmeasured.
    {"Insta360 X6", "Insta360", "z,-x,-y", nullptr, nullptr, 7680.0,
     {{3840, "8K"}, {3008, "6K"}, {1920, "4K"}, {0, nullptr}},
     {180.0, 0.0, 0.0}, 1.0, "_00_", nullptr, "LRV", "_01_", ".lrv",
     {1.0, 0.0, 0.0, 0.0}, 0.0, {}, nullptr, true, -0.5},
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
     {180.0, 0.0, 0.0}, 1000.0, "_00_", "_10_", "LRV", "_11_", ".insv",
     {1.0, 0.0, 0.0, 0.0}, 0.0, {}, nullptr, false, -2.3},
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
    // the per-clip seam fit (insv::EstimateSeamAlignment) over 17 sample
    // clips in every mode, term by term: the lenses 0.14 deg apart at the
    // seam on average, against 0.42 with the file's calibration alone, and
    // the sharpest seam of the corrections tried. One unit measured, its
    // calibration the same in every clip (docs/formats/antigravity-a1.md).
    {"Antigravity A1", "Antigravity", "-x,y,-z", nullptr, nullptr, 5248.0,
     {{3840, "8K"}, {2624, "5.2K"}, {1920, "4K"}, {0, nullptr}},
     {-90.0, 90.0, 0.0}, 1.0, nullptr, nullptr, "PRX", nullptr, ".prx",
     {0.5, 0.5, -0.5, 0.5}, 9.7, {{0.006, 0.238, 0.468}, {-0.000749, 0.000016}, 0.998714}, nullptr,
     true, -0.5},
};

} // namespace

std::span<const InsvModel> InsvModels() { return kModels; }

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

} // namespace insv
