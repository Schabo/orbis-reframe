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
// measured again rather than used. 2: each channel from the cells where it
// isn't clipped (1: only cells with no channel clipped).
constexpr int kLensColourMethod = 2;

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

// Where the difference changes round the seam within a frame -- a low sun's
// veiling light in one lens on one side of the seam -- a local correction
// follows it (SeamBlend::local): per azimuth bin, bounded by this, applied
// in the blend band and faded out over kLensColourLocalRampDeg past it.
constexpr double kLensColourLocalMaxLog = 0.25;
constexpr double kLensColourLocalRampDeg = 10.0;

// One frame's departure from the clip's lens colour `c` plus `residual`
// (log, per channel) per azimuth bin of the seam: the weighted median over
// the bin's usable cells into value[bin * 3 + channel], their weighted
// unclipped samples into weight[bin * 3 + channel] (0 = no usable cell for
// that channel). Each cell is weighted by how near it lies to where the
// lenses meet in its bin -- elevation meetRad[bin], a Gaussian of
// reachRad[bin] across the band -- since that is where a difference shows;
// null = all rows alike. A bin takes the cells all three channels are
// usable in, unless most of its weight is in cells with one clipped.
void LensColourPerBin(const SeamCutStats& stats, double bandRad, const LensColour& c,
                      const double residual[3], const float* meetRad, const float* reachRad,
                      float* value, float* weight);

// Veiling light: the sun in or near one lens scatters inside it and lays a
// haze over its picture -- light added, about the same amount on dark and
// bright, which no gain matches (a gain that matches the dark grass is far
// too strong on the sky). Where one lens has it at the seam, it is taken
// out of that lens (SeamBlend::veil) before the colour is matched: in
// linear light (the picture's levels to the power kLensVeilGamma, near
// enough for this), in the blend band and faded out over
// kLensColourLocalRampDeg past it, never below kLensVeilKeep of the light
// there, and eased off over the highlight knee as the gain is (a clipped
// sky stays clipped).
constexpr double kLensVeilGamma = 2.2;
constexpr double kLensVeilKeep = 0.3;
constexpr double kLensVeilMax = 0.5;
// Veiling light (green) at which the seam's fade takes the whole blend band
// (SeamCutTracker): what is left of it then fades across the band.
constexpr double kLensVeilWide = 0.04;

// A lens sample v (0..255) with `veil` (linear, >= 0) taken out.
double LensVeilRemove(double v, double veil);

// One frame's veiling light per azimuth bin of the seam into
// veil[bin * 3 + channel]: linear, lens 0 less lens 1 -- > 0 lens 0 has it,
// < 0 lens 1 -- bounded by kLensVeilMax; 0 where there is none or the bin
// can't tell it from a difference in gain (cells all about as bright).
void LensVeilPerBin(const SeamCutStats& stats, float* veil);

// `stats` with `veil` (LensVeilPerBin's layout) taken out of the cell means
// of the lens that has it.
void RemoveLensVeil(SeamCutStats& stats, const float* veil);

// One frame's departure from the clip's lens colour `c`: the median over its
// usable cells of the measured log ratio less L_c(e), per channel. False
// (and `out` untouched) when the frame has too few usable cells or looks
// like flare against the clip's spread.
bool LensColourResidual(const SeamCutStats& stats, double bandRad, const LensColour& c,
                        double out[3]);

} // namespace insv
