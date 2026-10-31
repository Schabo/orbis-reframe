// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <string>
#include <vector>

// How the two lenses of a clip differ in colour, measured from the video so
// the stitch can match them across the seam (insv::FitLensColour). Plain
// data, no dependencies: the project model stores it per source file.
//
// The lenses of one camera expose together, but their colour still differs
// by a few per cent -- white balance, and a rim falloff that isn't the same
// for both lenses, in colour too. Measured on the A1 and X5, that difference
// is fixed within a clip and changes from clip to clip, so it is measured
// once per clip, over frames spread across it, rather than per frame: a
// per-frame match from what both lenses see at the seam is thrown off by
// flare on one lens, and makes a step where the lenses agree.
//
// Per channel c (R, G, B), the log ratio lens 0 / lens 1 at elevation e off
// the seam circle (towards lens 0, degrees) is
//
//     L_c(e) = logRatio[c] + slope[c] * clamp(e, -band, band)
//
// with band the stitch's blend half-width: across the band it follows the
// measured slope, beyond it it holds the band edge's value, so the match is
// continuous over the whole of each lens. Half of it goes to each lens: lens
// 0 by exp(-L / 2), lens 1 by exp(L / 2), both to their geometric mean.

namespace insv {

struct SeamCutStats;

struct LensColour {
    double logRatio[3] = {0, 0, 0}; // log(lens 0 / lens 1) on the seam circle, R G B
    double slope[3] = {0, 0, 0};    // its change per degree towards lens 0
    // The share of seam cells far off the frame's own colour ratio in a
    // typical frame of the clip (0..1): flare and reflections push it up, but
    // some lenses (the ONE X2's) are that uneven everywhere. A frame well
    // above it is taken for flare (LensColourResidual).
    double spread = 0;

    bool operator==(const LensColour&) const = default;
};

// The estimator that measured a stored lens colour. One from an older one is
// measured again rather than used.
constexpr int kLensColourMethod = 1;

// Most the lenses' log ratio is matched by (per channel, either way; each
// lens gets half), and the most the per-frame residual may add to the
// clip's. The bound also keeps a channel rising through the highlight knee.
constexpr double kLensColourMaxLog = 0.3;
constexpr double kLensColourResidualMaxLog = 0.15;

// Highlights: a lens's gain eases to 1 as a channel rises from
// kLensColourKneeLo to kLensColourKneeHi (0..255), so a clipped sky stays the
// same from both lenses instead of splitting into 255 and a few levels less.
constexpr double kLensColourKneeLo = 192.0;
constexpr double kLensColourKneeHi = 250.0;

// L_c(e) (log lens 0 / lens 1) for elevation eRad (radians) in a blend band
// of half-width bandRad, plus `residual` (log, per channel; null = none).
void LensColourLog(const LensColour& c, double eRad, double bandRad, const double* residual,
                   double out[3]);

// The gain on a lens's channel of value v (0..255) for a log gain lg: exp(lg),
// eased to 1 over the highlight knee.
double LensColourGain(double lg, double v);

struct LensColourFit {
    bool ok = false;
    LensColour colour;
    int frames = 0;      // frames used
    int flareFrames = 0; // frames left out for flare
    int cells = 0;       // seam cells the fit stands on
    std::string message; // what was measured, or why not, for the log
};

// Fits the clip's lens colour from per-frame seam statistics (the lenses
// sampled raw, without any colour match; SeamCutStats rows across a blend
// band of half-width bandRad). Robust: frames with flare (many cells far
// off the frame's ratio) are left out, cells the lenses disagree on, clipped
// or dark ones are too, and per row across the band the median over the
// rest is taken before the line is fitted.
LensColourFit FitLensColour(const std::vector<const SeamCutStats*>& frames, double bandRad);

// One frame's departure from the clip's lens colour `c`: the median over its
// usable cells of the measured log ratio less L_c(e), per channel. False
// (and `out` untouched) when the frame has too few usable cells or looks
// like flare against the clip's spread.
bool LensColourResidual(const SeamCutStats& stats, double bandRad, const LensColour& c,
                        double out[3]);

} // namespace insv
