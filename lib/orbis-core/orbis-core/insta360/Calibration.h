// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <string>
#include <vector>

// Parses the Insta360 `.insv` lens calibration: the extended per-lens MEI string
// (orbiscore::ReadInsvExtendedCalibrationString, orbis-core/insta360/InsvTrailer.h) that every
// X5 clip carries, or the shorter `offset_v3` form older models carry. The
// trailer's short "Parameters" maker-note string (InsvMakerNotes::parameters,
// docs/formats/insta360-x5.md §5.2) is not parsed: it describes an equidistant
// lens model, and the stitch uses MEI only.

namespace orbiscore {

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

// How a lens maps an off-axis angle onto its sensor (LensProjection has the
// formulas). The coefficients are LensCalibExt's either way: fx fy cx cy, the
// radial k1..k4, the tangential p1 p2.
enum class LensModel {
    // Mei's unified omnidirectional model (xi, a polynomial in r^2,
    // thin-prism s1..s4 besides). Insta360's.
    Mei,
    // Equidistant with a polynomial in the angle (Kannala-Brandt):
    // theta_d = theta (1 + k1 theta^2 + ... + k5 theta^10), no xi, no
    // thin-prism terms; k5 comes beside the calibration. DJI's
    // (docs/formats/dji-360.md §3).
    ThetaPoly,
};

// Where a lens sits in the camera, which the camera's reader knows (the
// calibration gives each lens's small deviation from it). The stitch places
// lens i, in its lens frame (x and y the calibration image's negated, z the
// optical axis), by
//
//   lens -> rig = mount * Rz(roll - rollZeroDeg) * Rx(-yaw) * Ry(-pitch)
//
// with yaw / pitch / roll the calibration's (LensCalibExt, degrees) and
// `mount` row-major. The seam fine-tune adds to those three angles. A reader
// with the whole pose as one rotation puts it in `mount` and the
// calibration's angles at 0.
struct LensPose {
    double mount[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double rollZeroDeg = 0.0;
};

// Insta360's (lens 0 or 1): the roll counted from 90, a fixed sensor clock
// every model stores for both lenses, and lens 1 a half turn about the up
// axis from lens 0.
LensPose Insta360LensPose(int lens);

// Where a lens's picture shows part of its own camera or carrier (a drone's
// body reaching into the overlap), which the camera's reader knows. The
// stitch uses the other lens there, and measures nothing from it -- the seam
// cut, flow, colour, veiling light and depth all compare the lenses on the
// scene.
//
// The lens's clear radius, degrees off its axis, sampled evenly round its
// picture: sample k at the image angle k * 360 / size degrees, counted from
// the calibration image's +u axis towards +v (u right, v down). 0 where the
// picture is clear out to the edge of the field. Empty: no mask.
struct LensMask {
    std::vector<double> clearDeg;
    bool empty() const { return clearDeg.empty(); }
    // The clear radius at image angle azRad, radians: the smaller of the two
    // samples either side, so an edge between samples isn't cut; a very
    // large value where both are clear. Only for a non-empty mask.
    double clearRadAt(double azRad) const;
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

} // namespace orbiscore
