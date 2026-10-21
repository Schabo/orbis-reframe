// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvModels.h"

namespace insv {
namespace {

const InsvModel kModels[] = {
    {"Insta360 X5", "z,-x,-y", "ProtectorS", "ProtectorA", 5312.0,
     {{3840, "8K"}, {2880, "5.7K"}, {1920, "4K"}, {0, nullptr}},
     1.0, nullptr, "_01_", ".lrv"},
    // IMU axes: the X5's, unconfirmed -- it needs a clip with deliberate
    // rotations (docs/formats/insta360-x6.md). File names: taken to be the
    // X5's (one .insv + .lrv per take; the proxy's exact name not checked).
    {"Insta360 X6", "z,-x,-y", nullptr, nullptr, 7680.0,
     {{3840, "8K"}, {3008, "6K"}, {1920, "4K"}, {0, nullptr}},
     1.0, nullptr, "_01_", ".lrv"},
    // A lens per file at 5.7K, proxy LRV_…_11_….insv, telemetry in ms, no
    // lens-guard profiles (docs/formats/insta360-one-x2.md §1, §6, §8). IMU
    // axes: the X5's, not measured yet.
    {"Insta360 ONE X2", "z,-x,-y", nullptr, nullptr, 2880.0,
     {{2880, "5.7K"}, {0, nullptr}, {0, nullptr}, {0, nullptr}},
     1000.0, "_10_", "_11_", ".insv"},
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
