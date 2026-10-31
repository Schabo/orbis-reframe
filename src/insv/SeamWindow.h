// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "FisheyeProjection.h"
#include "LensColour.h"
#include "SeamCut.h"
#include "SeamDepth.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

// The depth-aware seam's state at a frame -- the depth table and the blend
// across the band with its colour correction -- as a function of the
// measurements of the frames around it only.
//
// Each frame is measured once (the depth cost volume and the cut statistics,
// SeamDepth.h / SeamCut.h). The state at frame N averages the frames within
// a window either side of N -- the depth data term and the cut's
// disagreement over +-depthHalfMs / cutHalfMs, the veiling light and the
// local colour correction over +-localHalfMs, each frame's colour residual
// over +-colourHalfMs -- and solves once, with no memory of an earlier
// state: whichever way a frame is reached (played into, stepped back to,
// seeked to, exported), the same frames give the same state, and frames on
// both sides outvote a one-frame outlier without lagging behind.
//
// The cut statistics are measured on a picture warped by a depth table: the
// depth solve over the frames in the depthHalfMs before the frame
// (measureTable), so that too depends only on frames a fixed distance away.
// A state at N therefore needs the frames from N - lookBackMs() to
// N + lookAheadMs(): an analysis of a stretch starts that much before it.
//
// Played forward with no frames after the current one, solve() gives the
// state from the frames so far (one-sided); a full set comes from a pass
// over the clip (stitch::ClipSeamAnalysis). The state is held in half
// precision, as that cache stores it, so a state solved here and one read
// back from the cache are the same. Dependency-free (insv lib).

namespace insv {

struct SeamWindowTuning {
    double depthHalfMs = 150.0;
    double cutHalfMs = 150.0;
    double localHalfMs = 40.0;
    double colourHalfMs = 500.0;
    SeamDepthTuning depth;
    SeamCutTuning cut;
};

// What the stitch needs of the seam for one frame.
struct SeamState {
    float q[kSeamDepthBins] = {}; // StitchGpuParams::seamQ
    SeamBlend blend;              // StitchGpuParams::seamBlend
    float depthConf = 0;          // the depth measurement's mean confidence, for diagnostics
};

// The state rounded to half precision and back (the cache's precision).
void QuantiseSeamState(SeamState& s);
uint16_t FloatToHalf(float f);
float HalfToFloat(uint16_t h);

class SeamWindow {
public:
    // The lenses' baseline (FisheyeStitcher::baselineM) and the blend band's
    // half-width (StitchGpuParams::bandRad). Forgets the frames.
    void configure(double baselineM, double bandRad);
    // The clip's lens colour, which the residuals are measured against.
    void setLensColour(const LensColour& c);
    // Forget the frames (a seek, a new clip).
    void reset();
    bool empty() const { return m_frames.empty(); }
    int64_t latestPts() const { return m_frames.empty() ? INT64_MIN : m_frames.back().pts; }

    // The depth table to measure the cut statistics of the frame at ptsMs
    // with: the depth solve over the frames before it within depthHalfMs (all
    // 0 with none).
    void measureTable(int64_t ptsMs, float* q) const;
    // A frame's measurements: its cost volume and its cut statistics,
    // measured with measureTable(ptsMs). A frame not after the latest one
    // starts over.
    void add(int64_t ptsMs, const SeamCosts& costs, const SeamCutStats& stats);
    // The state at ptsMs from the frames held around it; false (and `out` a
    // plain cross-fade) with none within depthHalfMs.
    bool solve(int64_t ptsMs, SeamState& out) const;
    // Drops the frames before ptsMs.
    void dropBefore(int64_t ptsMs);

    // How far before / after a frame the frames its state depends on lie.
    int64_t lookBackMs() const;
    int64_t lookAheadMs() const;

    SeamWindowTuning tuning;

private:
    struct Frame {
        int64_t pts = 0;
        std::unique_ptr<SeamDepthFrame> depth;
        std::vector<float> dis;  // SeamCutDisagreement
        std::unique_ptr<SeamCutStats> stats;
        std::vector<float> veil; // LensVeilPerBin
        bool residualOk = false;
        double residual[3] = {0, 0, 0};
    };
    std::deque<Frame> m_frames;
    double m_baselineM = 0.032;
    double m_bandRad = 0.0;
    LensColour m_colour;
};

} // namespace insv
