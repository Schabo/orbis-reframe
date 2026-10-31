// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "FisheyeProjection.h"
#include "Image.h"

#include <cstdint>
#include <string>
#include <vector>

// Depth-aware seam: per frame, find the stitch distance along the seam circle
// at which the two lenses agree, so near content (the bike, the rider, the
// stick, the road close by) isn't doubled.
//
// The lenses sit a baseline B apart along their common axis (X5 32 mm, A1
// 71 mm), so a point at distance D on the seam is seen by the two lenses
// ~B/D rad apart, along the elevation (off-seam) direction. For each of
// kSeamDepthBins azimuth bins around the seam circle and each candidate
// inverse distance q, a patch of the blend band is sampled from both lenses,
// each from its own centre (FisheyeStitcher::lensRay), and scored by NCC of
// the grey values once each lens's brightness plane is taken out
// (SeamPatchScore). That cost volume is measured on the GPU
// (fisheye_stitch_common.glsl, pass 2) or by MeasureSeamCosts on the CPU --
// the same sample pattern, see below.
//
// SeamDepthData turns a frame's costs into a confidence-weighted data term
// (texture, the best match, and how much better it is than any match away
// from it); SolveSeamDepth turns the data term averaged over the frames
// around one (insv::SeamWindow) into the per-bin q table the stitch uses
// (StitchGpuParams::seamQ).
//
// The average keeps a bin from flipping for a few frames to the road 3 m
// behind a crash bar 0.6 m away when the road's texture matches a little
// better; its neighbours, steadier now too, carry the bar's depth across.
// Dependency-free (insv lib).

namespace insv {

// Candidate inverse distances, 1/m: q_k = kSeamQMin + k * step. Slightly
// negative values let the table take up a small residual misalignment (it
// looks like a disparity of the "wrong" sign); 3.2 is ~0.3 m.
constexpr int kSeamDepthCands = 40;
constexpr double kSeamQMin = -0.5;
constexpr double kSeamQMax = 3.2;
inline double SeamCandQ(int k)
{
    return kSeamQMin + (kSeamQMax - kSeamQMin) * k / (kSeamDepthCands - 1);
}

// The shift between the two lenses the table may take, radians (baseline x
// q): the X5's candidate range (32 mm, -0.5 .. 3.2 1/m; -0.9 .. 5.9 deg). A
// camera with a wider baseline reaches less near -- the A1's 71 mm stops at
// ~0.7 m -- because a bigger shift than this against the +-kSeamPatchElDeg
// patch is a match with a different stretch of the scene, not a nearer one.
constexpr double kSeamShiftMinRad = kSeamQMin * 0.032;
constexpr double kSeamShiftMaxRad = kSeamQMax * 0.032;

// Sample pattern of one bin's patch: kSeamPatchAz x kSeamPatchEl directions,
// azimuth evenly across the bin, elevation evenly across
// +-kSeamPatchElDeg (sample centres). Mirrored by the GPU pass.
constexpr int kSeamPatchAz = 16;
constexpr int kSeamPatchEl = 16;
constexpr double kSeamPatchElDeg = 3.0;

// A patch flatter than this (grey variance, levels^2) has no NCC (0): on a
// blown-out or flat patch the variance is rounding noise -- the GPU's float
// sums make it large enough to give NCC far outside [-1, 1]. Mirrored by the
// GPU pass.
constexpr double kSeamMinVariance = 0.25;

// A patch's grey values are scored without their plane (a + b az + c el,
// fitted per lens): what's matched is texture, not shading. Glare from a low
// sun on the seam is a smooth gradient that each lens draws differently; a
// gradient lines up with itself at any shift along it, so plain NCC rises
// steadily towards the nearest candidate and outscores the scene's own weaker
// match at its true distance, and the seam bends the far scene round the sun.
//
// The plane comes out exactly from running sums, so the GPU pass stays one
// reduction. The sums, in this order: n, az, el, az^2, el^2, az el; x, x az,
// x el, x^2 (lens 0); y, y az, y el, y^2 (lens 1); x y -- az and el the
// sample's indices less 7.5 (centred), x and y its grey (0..255) in each lens.
constexpr int kSeamPatchSums = 15;

inline void SeamPatchAccumulate(double* sums, int s, double x, double y)
{
    const double a = s % kSeamPatchAz - 0.5 * (kSeamPatchAz - 1);
    const double e = s / kSeamPatchAz - 0.5 * (kSeamPatchEl - 1);
    const double v[kSeamPatchSums] = {1,     a,     e,     a * a, e * e, a * e, x,    x * a,
                                      x * e, x * x, y,     y * a, y * e, y * y, x * y};
    for (int i = 0; i < kSeamPatchSums; ++i)
        sums[i] += v[i];
}

// The sums -> NCC of the two lenses' plane residuals (0 when either is
// flatter than kSeamMinVariance; -2 with fewer than a quarter of the samples)
// and the texture, the smaller residual standard deviation. Mirrored by the
// GPU pass.
void SeamPatchScore(const double sums[kSeamPatchSums], float& ncc, float& texture);

// One frame's cost volume.
struct SeamCosts {
    // NCC of the two lenses' grey patches (planes out, SeamPatchScore), bin
    // b, candidate k; < -1.5 = no usable samples (outside the lenses).
    float ncc[kSeamDepthBins][kSeamDepthCands];
    // min of the two lenses' grey standard deviation about their planes
    // (0..255 levels) -- how much texture there is to match.
    float texture[kSeamDepthBins][kSeamDepthCands];
};

// Seam patch sample (bin, sample index s = el * kSeamPatchAz + az) ->
// azimuth, elevation (radians). The GPU pass computes the same.
void SeamPatchSample(int bin, int s, double& phi, double& e);

// CPU cost volume for one decoded lens pair (full-res RGB), with `st`
// configured for this clip (depthSeam need not be on). Multithreaded.
void MeasureSeamCosts(const FisheyeStitcher& st, const Image& lens0, const Image& lens1,
                      SeamCosts& out);

struct SeamDepthTuning {
    double texLo = 3.0, texHi = 10.0;  // grey std: no confidence .. full confidence
    double nccLo = 0.3, nccHi = 0.6;   // best NCC: no confidence .. full
    // Uniqueness: the best NCC less the best one more than uniqueAwayDeg of
    // shift away from it -- no confidence .. full; none when the best is at
    // the end of the shift range. A match as good elsewhere (a smooth ramp,
    // a repeated texture, scattered peaks) says nothing about the distance.
    double uniqueLo = 0.05, uniqueHi = 0.2;
    double uniqueAwayDeg = 1.0;
    double smooth = 0.25;              // cost per 1/m of q change between neighbour bins
    double smoothCap = 1.0;            // ... truncated at this q change (depth edges)
    double farPrior = 0.01;            // cost per 1/m of |q| (textureless -> far)
};

// One frame's depth data term: per bin and candidate, the bin's confidence
// x (1 - NCC), weighted before frames are averaged, so a frame that can't
// tell (blown-out sky, a plain wall) adds nothing there whatever its NCC
// values say.
struct SeamDepthFrame {
    float data[kSeamDepthBins][kSeamDepthCands];
    float conf[kSeamDepthBins];   // the bin's confidence, 0..1
    float unique[kSeamDepthBins]; // its best NCC less the best away from it
                                  // (0 with the best at the end of the
                                  // range, 2 with nothing away)
    float meanConf = 0;
};

// `baselineM`: the lenses' (FisheyeStitcher::baselineM), which turns a
// candidate's q into the shift between the lenses for the shift range
// (kSeamShiftMinRad .. kSeamShiftMaxRad) and the uniqueness.
void SeamDepthData(const SeamCosts& costs, double baselineM, const SeamDepthTuning& tuning,
                   SeamDepthFrame& out);

// The per-bin inverse stitch distance (1/m, kSeamDepthBins values) from a
// data term (SeamDepthFrame::data's layout, averaged over frames): a 1-D
// dynamic programme along the azimuth (smooth, depth edges allowed, a pull
// towards far where nothing is known), sub-candidate refinement and a light
// smoothing. A function of `data` alone.
void SolveSeamDepth(const float* data, double baselineM, const SeamDepthTuning& tuning, float* q);

} // namespace insv
