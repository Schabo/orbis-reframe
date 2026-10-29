// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <span>
#include <string>

// What differs between models that write Insta360's `.insv` (Insta360's own,
// and the Antigravity A1) and isn't in the file: measured or documented per
// model, one row each (docs/formats/). Everything a file does say is read
// from it instead.

namespace insv {

struct InsvModel {
    const char* name;  // the trailer's model field, "Insta360 X5"
    const char* maker; // "Insta360"

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

    // How the lenses sit in the picture: the turn from the stitch's lens
    // frame to the sphere's forward and up, as yaw, pitch, roll in degrees
    // (StitchParams::outYawDeg / outPitchDeg / outRollDeg). The calibration
    // places the lenses relative to each other; how the camera is held isn't
    // in the file. Insta360's cameras: {180, 0, 0}, lens 0's opposite
    // forward and the lenses level. The Antigravity A1 carries its lenses
    // looking down and up.
    double mountDeg[3];

    // The trailer's telemetry clock tick (IMU, exposure and field 24
    // timestamps), in microseconds: 1 on the X5 / X6, 1000 on the ONE X2,
    // whose stamps are milliseconds (docs/formats/insta360-one-x2.md §8).
    double telemetryTickUs;

    // File names on the card, taken from the clip's own (`VID_…`, the file
    // with the trailer). `clipToken`: the lens token in the clip's name
    // (`_00_`), null when the names carry none (the A1's `VID_<date>_<time>
    // _<n>.insv`). `secondLensToken`: the token that names the second lens's
    // file in its place when the clip's file holds one lens only (null: the
    // model always writes both lenses into one file). The proxy's prefix,
    // token in place of `clipToken` (null with it) and extension:
    // `LRV_…_01_….lrv` on the X5, `PRX_….prx` on the A1.
    const char* clipToken;
    const char* secondLensToken;
    const char* proxyPrefix;
    const char* proxyToken;
    const char* proxyExt;

    // How a flight controller's attitude track (InsvTelemetry::attitude)
    // turns into "up" for the IMU: body <- IMU = Ry(attitudeTiltDeg) *
    // R(imuMount)^T, about the body's right axis. `imuMount` is field 69's
    // value {w, x, y, z}, used only when a file with an attitude track
    // doesn't carry the field. The tilt is the camera's pitch in the airframe,
    // which the file doesn't say. Unused without an attitude track ({1, 0, 0,
    // 0}, 0 on the cameras).
    double imuMount[4];
    double attitudeTiltDeg;
};

// Every row.
std::span<const InsvModel> InsvModels();

// The row for `model` (the trailer's model field). A model without its own
// row gets the X5's, and `*known` (if given) is set false so the caller can
// say so.
const InsvModel& FindInsvModel(const std::string& model, bool* known = nullptr);

} // namespace insv
