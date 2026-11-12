// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "SeamCorrection.h"

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

    // The correction the model's files need on top of their own lens
    // calibration, in the per-clip seam fit's terms (insv::SeamCorrection:
    // lens 1's turn and centre, both lenses' radius), as offsets from the
    // file's values, so each camera keeps its own calibration under it
    // (StitchParams::modelCorrection): measured where the file's calibration
    // alone keeps the lenses apart at the seam on every clip of the model.
    // Not applied over a chart calibration, which replaces the file's. None
    // ({}) on the cameras whose calibration lines the lenses up as it is.
    SeamCorrection lensCorrection;

    // The axes of the orientation a timelapse records in place of the gyro
    // (record 0x1800, InsvTelemetry::timelapse_quats) -> the stitched sphere,
    // a signed permutation as imuAxes. Null when unknown: the timelapse then
    // isn't stabilized. X5: "y,z,x", measured on the six sample timelapses
    // (2026-11-05) -- body +z is world up within 2-4 deg, the bike's lean
    // lies across the travel the picture shows only under this map, and
    // its turns go the way the GPS course does (docs/formats/insta360-x5.md
    // §3.5).
    const char* timelapseQuatAxes;

    // When a frame's picture was taken, from the frame's time in the
    // trailer (field 24 and the exposure record) and the IMU's latency
    // (field 28): `midExposure` true, each sensor row at the middle of its
    // exposure, the centre row at the frame's time + field 28 +
    // captureLagMs - that frame's exposure time (record 0x400) / 2; false,
    // at the frame's time + field 28 + captureLagMs whatever the exposure.
    // X5 and A1: true, -0.5 ms, by the per-clip sync measurement
    // (stitch::MeasureClipSync) on three windows of every sample clip
    // against the window's exposure and readout time: the best lag fell by
    // half of each ms of exposure on both (X5 0.3-10 ms on 19 clips, 0.31 ms
    // RMS left; A1 0.4-5 ms on 16, 0.10 ms), the readout time (6-21 ms)
    // moved it by nothing (docs/formats/insta360-x5.md §7.1).
    bool midExposure;
    double captureLagMs;

    // The longest file the camera writes, in seconds of video: a longer
    // recording goes on in another file (docs/formats/insta360-x5.md §1).
    // A last part about this long, with nothing beside it to continue it,
    // may have lost its continuation -- the first file doesn't say it goes
    // on. X5: 29 min 59 s, per Insta360 (its `_019` holds 1799.8 s). 0 when
    // not known: no such guess.
    double fileLimitSec;
};

// Every row.
std::span<const InsvModel> InsvModels();

// The row for `model` (the trailer's model field). A model without its own
// row gets the X5's, and `*known` (if given) is set false so the caller can
// say so.
const InsvModel& FindInsvModel(const std::string& model, bool* known = nullptr);

} // namespace insv
