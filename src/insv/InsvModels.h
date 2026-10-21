// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <string>

// What differs between Insta360 models and isn't in the file: measured or
// documented per model, one row each (docs/formats/). Everything a file does
// say is read from it instead.

namespace insv {

struct InsvModel {
    const char* name; // the trailer's model field, "Insta360 X5"

    // IMU sensor axis -> stitched sphere (+X right, +Y up, -Z forward), see
    // ImuTrack::axisMap. X5: "z,-x,-y", measured 2026-09-25 by fitting
    // essential-matrix camera rotation from raw renders against the
    // (correctly synced) gyro: three view directions of VID_20250904_193105
    // agree to a few degrees, and a gravity fit on another clip found the same
    // permutation (docs/formats/insta360-x5.md §6.3).
    const char* imuAxes;

    // The lens-profile names (0x101 field 145) for the standard and premium
    // lens guards. The X5's ProtectorA / ProtectorS / ProtectorAS carry
    // identical data; which letter means which guard is a guess (A = premium,
    // S = standard).
    // Null when the model's files carry no profile table (the X6: no field
    // 145, so the stitch takes every lens as bare).
    const char* standardGuard;
    const char* premiumGuard;

    // The focal reference window's width (LensCalibExt::focalRefW) for files
    // that don't carry field 27. The X5's 5312; the X6's 7680 (its clips
    // carry the field, docs/formats/insta360-x6.md).
    double focalRefW;

    // Capture modes by the square lens picture's width; unused entries are
    // {0, nullptr}.
    // 5.7K+ records the same stream shape as 5.7K, so it can't be told apart
    // (docs/formats/insta360-x5.md §3.3).
    struct CaptureMode {
        int lensWidth;
        const char* name; // "8K"
    };
    CaptureMode captureModes[4];

    // The capture mode named for a lens picture `lensWidth` wide, or "".
    const char* captureMode(int lensWidth) const;

    // The trailer's telemetry clock tick (IMU, exposure and field 24
    // timestamps), in microseconds: 1 on the X5 / X6, 1000 on the ONE X2,
    // whose stamps are milliseconds (docs/formats/insta360-one-x2.md §8).
    double telemetryTickUs;

    // File names on the card, taken from the clip's own (`VID_…_00_…`, the
    // file with the trailer): the token that names the second lens's file in
    // place of `_00_` when the clip's file holds one lens only (null: the
    // model always writes both lenses into one file), and the proxy's token
    // and extension (`LRV_…_01_….lrv` on the X5).
    const char* secondLensToken;
    const char* proxyToken;
    const char* proxyExt;
};

// The row for `model` (the trailer's model field). A model without its own
// row gets the X5's, and `*known` (if given) is set false so the caller can
// say so.
const InsvModel& FindInsvModel(const std::string& model, bool* known = nullptr);

} // namespace insv
