// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <orbis-core/stitch/LensModel.h>

#include <string>

// Parses the Insta360 `.insv` lens calibration: the extended per-lens MEI string
// (orbiscore::ReadInsvExtendedCalibrationString, InsvTrailer.h) that every
// X5 clip carries, or the shorter `offset_v3` form older models carry, into
// the stitch's lens types (orbis-core/stitch/LensModel.h). The
// trailer's short "Parameters" maker-note string (InsvMakerNotes::parameters,
// docs/formats/insta360-x5.md §5.2) is not parsed: it describes an equidistant
// lens model, and the stitch uses MEI only.

namespace orbiscore {

// Insta360's (lens 0 or 1): the roll counted from 90, a fixed sensor clock
// every model stores for both lenses, and lens 1 a half turn about the up
// axis from lens 0.
LensPose Insta360LensPose(int lens);

// Parses the extended "2_..." string into `out.lensesExt`. Accepts the raw
// underscore-delimited form or one with the underscores already turned into
// spaces, and tolerates the string being embedded in a longer buffer. Returns
// false and leaves `out` unchanged if the string doesn't hold `1 + 2*27` numeric
// tokens with both per-lens `xi`/`fx`/`fy`/`w` in a sane range -- so a caller can
// unconditionally try it on any candidate string.
bool ParseExtendedCalibrationString(const std::string& s, CameraCalibration& out);

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
bool ParseCalibrationV3String(const std::string& s, CameraCalibration& out);

} // namespace orbiscore
