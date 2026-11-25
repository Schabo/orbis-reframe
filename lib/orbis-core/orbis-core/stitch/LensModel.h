// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <utility>
#include <vector>

// What the stitch knows about a camera's lenses, whichever camera it is:
// each lens's calibration, its projection, where it sits in the camera and
// where its picture shows the camera itself. A camera's reader fills these
// from its file (Insta360's parsers are in orbis-core/insta360/Calibration.h).

namespace orbiscore {

// One lens's calibration, as the stitch uses it: intrinsics, the lens's
// small deviation from its place in the camera (LensPose), distortion, and
// the frame the pixel coordinates live in. The fields follow Insta360's
// *extended* per-lens calibration (the Mei/MEI unified omnidirectional
// model) that Insta360 Studio actually stitches with; another camera's reader
// fills the same fields from its own file (LensModel says which projection
// the coefficients are for). Insta360's is NOT in the short "p2_" maker-note
// string -- it lives as plaintext
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

// Exchanges the two lenses' radial models: focal length (fx, fy) and
// radial distortion (k1-k4). What a camera whose strings carry each lens's
// in the other's place needs (orbiscore::InsvModel::radialModelsExchanged).
inline void ExchangeRadialModels(LensCalibExt& a, LensCalibExt& b)
{
    std::swap(a.fx, b.fx);
    std::swap(a.fy, b.fy);
    for (int i = 0; i < 4; ++i)
        std::swap(a.k[i], b.k[i]);
}

// A camera's lenses' calibrations, lens 0 first.
struct CameraCalibration {
    std::vector<LensCalibExt> lensesExt; // extended MEI form; empty until parsed
};

} // namespace orbiscore
