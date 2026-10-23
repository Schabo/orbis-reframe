// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "FisheyeProjection.h"
#include "Image.h"

#include <cstdint>
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
// MeasureSeamCutStats on the CPU, with the previous frame's depth table, and
// averaged over ~150 ms, so the cut and its width don't flicker.
// Dependency-free (insv lib).

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
        float pad = 0;
    };
    Cell cell[kSeamDepthBins][kSeamCutRows];
};

// CPU statistics for one decoded lens pair (full-res RGB), each lens warped
// by `seamQ` (kSeamDepthBins values; the depth table the frame before).
// Multithreaded.
void MeasureSeamCutStats(const FisheyeStitcher& st, const Image& lens0, const Image& lens1,
                         const float* seamQ, SeamCutStats& out);

struct SeamCutTuning {
    double memoryMs = 150.0;  // time constant of the statistics' running average
    double texFloor = 4.0;    // disagreement = zsad / (texture + texFloor)
    double agreeLo = 0.35;    // disagreement: narrow fade up to here ...
    double agreeHi = 0.7;     // ... the full cross-fade from here
    double rowChange = 0.08;  // DP cost per degree of row change between neighbour bins
    double offCentre = 0.01;  // cost per degree away from the seam circle
    double stay = 0.15;       // cost for leaving last frame's row in a bin
    double widthSigma = 1.0;  // smoothing of the fade width along the azimuth, bins
    int64_t resetGapMs = 250; // a jump this big (or backwards) starts over
};

class SeamCutTracker {
public:
    SeamCutTracker();

    // Forget the history (a seek, a new clip); the blend goes back to the
    // plain cross-fade.
    void reset();

    // Folds one frame's statistics in (a gap or a jump back in ptsMs resets
    // first). bandRad: the stitch's blend half-width (StitchGpuParams).
    void update(const SeamCutStats& stats, int64_t ptsMs, double bandRad);

    const SeamBlend& blend() const { return m_blend; }
    bool hasHistory() const { return m_has; }

    // The last update's per-bin disagreement at the chosen row and the fade
    // width share (0 narrow .. 1 the full band), for diagnostics.
    const std::vector<float>& lastDisagreement() const { return m_dis; }
    const std::vector<float>& lastWidth() const { return m_width; }

    SeamCutTuning tuning;

private:
    SeamBlend m_blend;
    // The running average of each cell's disagreement (bin-major,
    // kSeamCutRows per bin), valid while m_has.
    std::vector<float> m_avgDis;
    std::vector<int> m_row; // last frame's row per bin
    std::vector<float> m_dis, m_width;
    bool m_has = false;
    int64_t m_lastPts = 0;
};

} // namespace insv
