// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvModels.h"

namespace insv {
namespace {

const InsvModel kModels[] = {
    {"Insta360 X5", "z,-x,-y", "ProtectorS", "ProtectorA", 5312.0,
     {{3840, "8K"}, {2880, "5.7K"}, {1920, "4K"}, {0, nullptr}}},
    // IMU axes: the X5's, unconfirmed -- it needs a clip with deliberate
    // rotations (docs/formats/insta360-x6.md).
    {"Insta360 X6", "z,-x,-y", nullptr, nullptr, 7680.0,
     {{3840, "8K"}, {3008, "6K"}, {1920, "4K"}, {0, nullptr}}},
};

} // namespace

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
