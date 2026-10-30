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
// SeamDepthTracker turns a frame's costs into the per-bin q table the stitch
// uses (StitchGpuParams::seamQ): confidence-weighted costs averaged over the
// last ~150 ms, a 1-D dynamic programme along the azimuth (smooth, but depth
// edges allowed), a pull towards the previous frame's table, sub-candidate
// refinement, and a light temporal blend.
//
// The average keeps a bin from flipping for a few frames to the road 3 m
// behind a crash bar 0.6 m away when the road's texture matches a little
// better; its neighbours, steadier now too, carry the bar's depth across.
// Longer averages hold more, but follow something near really moving onto
// the seam later (~0.4 s at 500 ms, where 150 ms costs ~0.1 s). The costs
// are weighted by confidence before averaging, so a frame that can't tell
// (blown-out sky) adds nothing. Dependency-free (insv lib).

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
    double smooth = 0.25;              // cost per 1/m of q change between neighbour bins
    double smoothCap = 1.0;            // ... truncated at this q change (depth edges)
    double temporal = 0.08;            // cost per 1/m away from the previous frame's q
    double farPrior = 0.01;            // cost per 1/m of |q| (textureless -> far)
    double blend = 0.5;                // temporal blend of the new table (1 = no memory)
    double costMemoryMs = 150.0;       // time constant of the data term's running average (0 = none)
    int64_t resetGapMs = 250;          // a jump this big (or backwards) starts over
};

class SeamDepthTracker {
public:
    SeamDepthTracker();

    // Forget the history (a seek, a new clip); the table goes back to 0.
    void reset();

    // Folds one frame's costs in (a gap or a jump back in ptsMs resets first).
    void update(const SeamCosts& costs, int64_t ptsMs);

    // The current per-bin inverse stitch distance (1/m), kSeamDepthBins long.
    const float* table() const { return m_q; }
    bool hasHistory() const { return m_has; }

    // Mean confidence of the last update (0..1), for diagnostics.
    double lastConfidence() const { return m_lastConf; }

    SeamDepthTuning tuning;

private:
    float m_q[kSeamDepthBins];
    // The running average of the confidence-weighted data term (bin-major,
    // kSeamDepthCands per bin), valid while m_has.
    std::vector<float> m_avgData;
    bool m_has = false;
    int64_t m_lastPts = 0;
    double m_lastConf = 0;
};

} // namespace insv
