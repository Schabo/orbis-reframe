// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvModels.h"

namespace insv {
namespace {

const InsvModel kModels[] = {
    {"Insta360 X5", "Insta360", "z,-x,-y", "ProtectorS", "ProtectorA", 5312.0,
     {{3840, "8K"}, {2880, "5.7K"}, {1920, "4K"}, {0, nullptr}},
     {180.0, 0.0, 0.0}, 1.0, "_00_", nullptr, "LRV", "_01_", ".lrv"},
    // IMU axes: the X5's, unconfirmed -- it needs a clip with deliberate
    // rotations (docs/formats/insta360-x6.md). File names: taken to be the
    // X5's (one .insv + .lrv per take; the proxy's exact name not checked).
    {"Insta360 X6", "Insta360", "z,-x,-y", nullptr, nullptr, 7680.0,
     {{3840, "8K"}, {3008, "6K"}, {1920, "4K"}, {0, nullptr}},
     {180.0, 0.0, 0.0}, 1.0, "_00_", nullptr, "LRV", "_01_", ".lrv"},
    // A lens per file at 5.7K, proxy LRV_…_11_….insv, telemetry in ms, no
    // lens-guard profiles (docs/formats/insta360-one-x2.md §1, §6, §8). IMU
    // axes: "z,x,y", the X5's turned 180° about sphere X -- measured on both
    // sample takes by fitting the image rotation of four unstabilized views
    // against the gyro (doc §8).
    {"Insta360 ONE X2", "Insta360", "z,x,y", nullptr, nullptr, 2880.0,
     {{2880, "5.7K"}, {0, nullptr}, {0, nullptr}, {0, nullptr}},
     {180.0, 0.0, 0.0}, 1000.0, "_00_", "_10_", "LRV", "_11_", ".insv"},
    // A 360 drone writing Insta360's .insv (docs/formats/antigravity-a1.md):
    // both lenses in one file, the proxy PRX_….prx with the clip's own name,
    // no lens-guard profiles, field 27 in the file. The 5.2K and 4K lens
    // sizes are from the spec sheet (no clips yet). IMU axes: "-x,y,-z" in
    // the sphere its mount gives, measured on two flight stretches of turns
    // by fitting the image rotation of four unstabilized views against the
    // gyro (1.3 deg from the free fit, gain ~1; raw +y is also where the
    // accelerometer puts "up" in a hover).
    {"Antigravity A1", "Antigravity", "-x,y,-z", nullptr, nullptr, 5248.0,
     {{3840, "8K"}, {2624, "5.2K"}, {1920, "4K"}, {0, nullptr}},
     {-90.0, 90.0, 0.0}, 1.0, nullptr, nullptr, "PRX", nullptr, ".prx"},
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
