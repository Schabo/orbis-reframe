// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <string>
#include <vector>

// Parses the Insta360 `.insv` lens calibration: the extended per-lens MEI string
// (insv::ReadInsvExtendedCalibrationString, src/insv/InsvTrailer.h) that every
// X5 clip carries, or the shorter `offset_v3` form older models carry. The trailer's short "Parameters" maker-note string
// (InsvMakerNotes::parameters, docs/formats/insta360-x5.md §5.2) is not parsed: it
// only fed a legacy equidistant stitch model, removed once MEI was the default.

namespace insv {

// The *extended* per-lens calibration (the Mei/MEI unified omnidirectional
// model) that Insta360 Studio actually stitches with. It is NOT in the short
// "p2_" maker-note string -- it lives as plaintext
// in the .insv trailer (and the MISC/CameraNN/*.lrv.pb sidecar's base64 blob) as
// a 56-token "2_..." string: a leading count, then 27 values per lens, then one
// trailing token. Layout per lens (matches BenjaminHenriksson/insv-stitch
// x5_pipeline.py `_parse_extended_calibration`):
//
//   [ 0] xi        mirror parameter (X5: ~2.0)
//   [ 1] fx        focal length, sensor-resolution pixels
//   [ 2] fy
//   [ 3] cx        principal point, sensor-resolution pixels (side-by-side
//   [ 4] cy          layout: lens 1's cx is offset by half the combined width)
//   [ 5] yaw       extrinsic, degrees
//   [ 6] pitch     extrinsic, degrees
//   [ 7] roll      extrinsic, degrees -- UNUSED for the X5 (sensor clock)
//   [ 8] tx        lens offset from the camera centre
//   [ 9] ty
//   [10] tz
//   [11..14] k1 k2 k3 k4     radial distortion (r^2 .. r^8)
//   [15]     (reserved)
//   [16..17] p1 p2           tangential distortion
//   [18..21] s1 s2 s3 s4     thin-prism distortion
//   [22..23] (reserved)
//   [24] w        combined frame width  the coords live in (X5: 10752)
//   [25] h        combined frame height (X5: 5376)
//   [26] type
struct LensCalibExt {
    double xi = 0;
    double fx = 0, fy = 0;
    double cx = 0, cy = 0;
    double yaw = 0, pitch = 0, roll = 0; // degrees
    double tx = 0, ty = 0, tz = 0;
    double k[4] = {0, 0, 0, 0}; // radial
    double p[2] = {0, 0};       // tangential
    double s[4] = {0, 0, 0, 0}; // thin-prism
    double w = 0, h = 0;        // combined-frame dims the cx/cy live in
    int type = 0;
    // Not in the string: the width of the sensor window fx/fy are given in
    // (on Insta360 files the trailer's field 27, InsvMakerNotes::crop_dst_w).
    // The file's reader sets it; 0 = unknown, and the stitch refuses it.
    double focalRefW = 0;
};

struct InsvCalibration {
    std::vector<LensCalibExt> lensesExt; // extended MEI form; empty until parsed
};

// Parses the extended "2_..." string into `out.lensesExt`. Accepts the raw
// underscore-delimited form or one with the underscores already turned into
// spaces, and tolerates the string being embedded in a longer buffer. Returns
// false and leaves `out` unchanged if the string doesn't hold `1 + 2*27` numeric
// tokens with both per-lens `xi`/`fx`/`fy`/`w` in a sane range -- so a caller can
// unconditionally try it on any candidate string.
bool ParseExtendedCalibrationString(const std::string& s, InsvCalibration& out);

// Parses the 40-token `offset_v3` string (metadata field 54) into
// `out.lensesExt`: the same MEI model in a shorter layout, a leading count,
// then per lens
//
//   xi fx fy cx cy yaw pitch roll tx ty tz k1 k2 k3 p1 p2 w h type
//
// and one trailing token (docs/formats/insta360-one-x2.md §7). It has no k4
// and no thin-prism terms: those are 0. The ONE X2 carries only this one;
// the X5 carries both, and the 56-token string is the one to use there.
// Same sanity checks and contract as ParseExtendedCalibrationString.
bool ParseCalibrationV3String(const std::string& s, InsvCalibration& out);

} // namespace insv
