// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "FisheyeProjection.h"
#include "Image.h"
#include "LensColour.h"

#include <cstdint>
#include <memory>
#include <vector>

// Depth-aware seam, the blend across the band: where the two lenses meet and
// how wide the fade between them is, per azimuth bin (insv::SeamBlend).
//
// The depth table (SeamDepth.h) lines the lenses up at one distance per bin,
// but inside a bin the depth still varies -- a mirror stem in front of the
// road, a rack edge in front of the bike's tail. A wide cross-fade shows
// whatever doesn't line up as a ghost. So across the band, kSeamCutRows rows
// are compared, the two lenses warped by the depth table: the zero-mean grey
// difference of a small patch around each row, against its texture. A dynamic
// programme along the azimuth picks the row per bin where the lenses agree
// best, and the fade there is narrow (kSeamCutFadeDeg).
//
// Where no row agrees -- a thin very close object the two lenses see against
// different backgrounds (a crash bar with the road behind it) -- a narrow
// fade would tear it, so the fade widens back to the plain cross-fade over
// the whole band. The statistics are measured on the GPU (pass 3 of
// fisheye_stitch_common.glsl, beside the depth cost volume) or by
// MeasureSeamCutStats on the CPU, with the depth table of the frames just
// before, and averaged over the frames around one (insv::SeamWindow), so the
// cut and its width don't flicker.
//
// The same rows measure the lenses' colour: each lens's mean R, G, B, each
// channel where neither lens has it clipped. Over a clip they give its lens
// colour (insv::FitLensColour); per frame, what is left of the difference after it,
// a small slow residual (SeamBlend::colour) for lenses that drift apart
// within a clip, per azimuth bin a faster local correction
// (SeamBlend::local) where the difference changes round the seam, and the
// veiling light a low sun adds to one lens there (SeamBlend::veil), taken out
// before the colour is matched. Dependency-free (insv lib).

namespace insv {

// Rows across the band, evenly from -SeamCutRowMax to +SeamCutRowMax.
constexpr int kSeamCutRows = 9;
// A row's patch: kSeamCutPatchAz x kSeamCutPatchEl directions, azimuth evenly
// across the bin, elevation evenly across +-kSeamCutPatchElDeg around the row
// (sample centres). Mirrored by the GPU pass (64 samples, one per invocation).
constexpr int kSeamCutPatchAz = 16;
constexpr int kSeamCutPatchEl = 4;
constexpr double kSeamCutPatchElDeg = 0.6;
// Half-width of the narrow fade, degrees.
constexpr double kSeamCutFadeDeg = 0.75;
// A sample with a channel above this (0..255) is clipped: its colour says
// nothing about the lenses' exposure.
constexpr double kSeamCutClip = 250.0;

// The outermost row's elevation for a blend band of half-width bandRad: the
// narrow fade and a margin stay inside the band. Radians, >= 0.
double SeamCutRowMax(double bandRad);
// Row r's elevation, radians, towards lens 0.
double SeamCutRowE(int r, double bandRad);
// Row patch sample (bin, row, sample index s = el * kSeamCutPatchAz + az) ->
// azimuth, elevation (radians). The GPU pass computes the same.
void SeamCutSample(int bin, int row, int s, double bandRad, double& phi, double& e);

// One frame's statistics, per bin and row.
struct SeamCutStats {
    struct Cell {
        float n = 0;       // samples both lenses see (of kSeamCutPatchAz * kSeamCutPatchEl)
        float zsad = 0;    // mean |(g0 - mean g0) - (g1 - mean g1)| of the grey, levels
        float texture = 0; // min of the two lenses' grey standard deviation, levels
        // Per channel R, G, B: of those, the samples where neither lens has
        // that channel above kSeamCutClip, and each lens's mean of it over
        // them, levels -- a sky clipped in blue still tells red and green.
        float nColour[3] = {};
        float mean[2][3] = {};
    };
    Cell cell[kSeamDepthBins][kSeamCutRows];
};

// CPU statistics for one decoded lens pair (full-res RGB), each lens warped
// by `seamQ` (kSeamDepthBins values; the depth table the frame before).
// Multithreaded.
void MeasureSeamCutStats(const FisheyeStitcher& st, const Image& lens0, const Image& lens1,
                         const float* seamQ, SeamCutStats& out);

struct SeamCutTuning {
    double texFloor = 4.0;    // disagreement = zsad / (texture + texFloor)
    double agreeLo = 0.35;    // disagreement: narrow fade up to here ...
    double agreeHi = 0.7;     // ... the full cross-fade from here
    double rowChange = 0.08;  // DP cost per degree of row change between neighbour bins
    double offCentre = 0.01;  // cost per degree away from the seam circle
    double widthSigma = 1.0;  // smoothing of the fade width along the azimuth, bins
    double localSigma = 3.0;        // the local correction's smoothing along the azimuth, bins
    double localMinSamples = 128.0; // unclipped samples near where the lenses meet, bin +-1, for all of it
};

// One frame's disagreement per cell (bin-major, kSeamCutRows per bin): the
// zero-mean grey difference against the texture, so a busy patch that
// matches isn't taken for an occlusion; a cell the lenses don't both see
// can't be chosen.
void SeamCutDisagreement(const SeamCutStats& stats, const SeamCutTuning& tuning, float* dis);

// The cut from a disagreement (SeamCutDisagreement's layout, averaged over
// frames): one row per bin by a circular DP along the azimuth, and the
// fade's width share per bin (0 narrow .. 1 the full band) from the
// disagreement at that row, smoothed along the azimuth. A function of `dis`
// alone.
void SolveSeamCut(const float* dis, double bandRad, const SeamCutTuning& tuning, int* row,
                  float* width);

// Where the lenses meet in each bin for a cut (`row`, `width`): the fade's
// centre (elevation, radians) and its reach (as wide as the fade, at least
// a row) -- where the colour is matched.
void SeamCutMeet(const int* row, const float* width, double bandRad, float* meetRad,
                 float* reachRad);

// One frame's departure from the clip's lens colour `c` with its own
// veiling light taken out (LensVeilPerBin / RemoveLensVeil, then
// LensColourResidual on both): false when it says nothing (too few usable
// cells, or flare -- judged before the veil is taken out).
bool SeamFrameResidual(const SeamCutStats& stats, double bandRad, const LensColour& c,
                       double out[3]);

// One frame's local correction round the seam (SeamBlend::local's layout,
// log): what is left per bin of `clean` (the frame's statistics, veiling
// light taken out) after the clip's colour and `residual`, measured where
// the lenses meet, smoothed along the seam from the bins that have data and
// faded to none where the bin and its neighbours have little of their own
// -- a stretch without its own data isn't given the next one's correction.
// Per channel: a sky clipped in blue still has red and green.
void SeamLocalCorrection(const SeamCutStats& clean, double bandRad, const LensColour& c,
                         const double residual[3], const float* meetRad, const float* reachRad,
                         const SeamCutTuning& tuning, float* local);

// The blend's cut and fade from the cut and the veiling light: where one
// lens has veiling light the fade takes the full band -- what is left of it
// past the seam (it grows towards the sun, the other lens can't say by how
// much) fades across the band instead of meeting the other lens on a line.
void SeamCutBlend(const int* row, const float* width, double bandRad, SeamBlend& blend);

// The fade's half-width for a share of the band (0 the narrow fade,
// kSeamCutFadeDeg, .. 1 the whole band), radians.
double SeamFadeRad(double share, double bandRad);
// The share a stretch's veiling light (its green, lens 0 less lens 1,
// linear) widens the fade to: the whole band from kLensVeilWide.
double SeamVeilShare(double veilGreen);

} // namespace insv
