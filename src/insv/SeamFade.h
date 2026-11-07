// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

// The new stitcher's fade between the lenses, with its seam colour blend
// and seam flow: narrow on the seam circle where the lenses show the same
// picture, the whole band where they don't.
//
// A fade across the whole band mixes both lenses where each is at its
// softest, out to the band's edge (+-7.5 deg on the A1), and softens the
// seam. With the colours matched and near things lined up by the flow, the
// lenses can meet in a narrow fade on the seam (kSeamCutFadeDeg, as the old
// stitcher's cut) instead. But where they still show different pictures --
// the haze and the rays the sun puts into one lens only, something close the
// flow can't line up -- a narrow fade turns the difference into a line along
// the seam, which the wide fade spreads out of sight. So per strip column
// the fade's share of the band (0 narrow .. 1 the whole band, SeamFadeRad)
// comes from how far apart the lenses are near the seam in the stitch's own
// picture: each lens's strip warped as the stitch samples it (by its share
// of the frame's flow, SeamFlowShare), patch means within kSeamFadeNearDeg
// of the seam, the colour blend's ratio applied (SeamFadeWindow::solve), the
// largest difference over the patches across the seam. It goes by the
// strips' fixed grid, not by any view, so every path stitches a frame alike.
//
// A frame's patch means are kept (SeamFadeFrame) with the flow's frames
// (kSeamFlowFrames); the share is the largest of theirs, the older ones
// counting less, so the fade widens at once and narrows over the window
// instead of flickering; a seek measures the frames before it, as the flow
// does. Then spread and smoothed along the seam, so the fade's width
// doesn't step from column to column. The strips are the raw ones, before
// the flow's anchor pattern. Mirrored in seam_flow.comp's fade passes.
// Dependency-free (insv lib).

#include "FisheyeProjection.h" // kSeamDepthBins
#include "SeamFlow.h"

#include <deque>
#include <vector>

namespace insv {

// Patch centres every kSeamFadeRowStep strip rows within this of the seam
// circle (degrees): where a narrow fade puts the lenses' difference.
constexpr double kSeamFadeNearDeg = 2.0;
constexpr int kSeamFadeRowStep = 2;
// A patch, strip pixels (~1.5 x 0.7 deg at kSeamFlowPxDeg): a ray or a near
// edge is a few of them, the strips' sampling noise averages out.
constexpr int kSeamFadePatchW = 11;
constexpr int kSeamFadePatchH = 5;
// The difference (grey levels, patch means, colour matched) up to which the
// fade stays narrow, and from which it takes the whole band.
constexpr double kSeamFadeLo = 12.0;
constexpr double kSeamFadeHi = 30.0;
// Along the seam: each column's share spread to the columns this far either
// side (the largest), then smoothed by a Gaussian of this sigma (degrees).
constexpr double kSeamFadeSpreadDeg = 1.5;
constexpr double kSeamFadeSmoothDeg = 1.5;

// The patch centres' rows on a grid: how many, and centre r's strip row.
int SeamFadeRows(const SeamFlowGrid& g);
int SeamFadeRowY(const SeamFlowGrid& g, int r);

// One frame's patch means of both lenses, each warped by its share of
// `field` -- the frame's own flow, before the window: a frame's means are
// then the same whatever came before it, so a seek's frames match
// playback's (the window's mean would differ by the frames before the
// seek). Per patch
// centre r and column x at [r * cols + x]; NaN where fewer than half the
// patch's pixels have both lenses.
struct SeamFadeFrame {
    SeamFlowGrid grid;
    std::vector<float> mean[2];
};
void MeasureSeamFadeFrame(const SeamFlowStrips& raw, const SeamFlowField& field, SeamFadeFrame& out);

// The fade's share per strip column from one frame's patch means and the
// seam colour blend's table (StitchGpuParams::seamColour's layout, log
// lens 0 / lens 1; each lens moved half of it, as the stitch does): the
// largest difference over the patches across the seam, from kSeamFadeLo
// (0) to kSeamFadeHi (1). Before the window and the smoothing.
void SeamFadeShares(const SeamFadeFrame& f, const float* seamColour, std::vector<float>& share);

// The last kSeamFlowFrames frames' patch means, as SeamFlowWindow holds
// their fields. Frames come in play order; a gap (a seek) is the caller's
// to reset().
class SeamFadeWindow {
public:
    void reset() { frames_.clear(); }
    void add(SeamFadeFrame f);
    int size() const { return static_cast<int>(frames_.size()); }
    // The share per column into `share` (cols values; empty with no frame):
    // frame k before the newest counts 1 - k / kSeamFlowFrames of its own,
    // the largest wins; spread and smoothed along the seam.
    void solve(const float* seamColour, std::vector<float>& share) const;

private:
    std::deque<SeamFadeFrame> frames_;
};

// The spread and the smoothing along the seam, in place (cols values, x
// wraps round): SeamFadeWindow::solve's last step, shared with the GPU's.
void SeamFadeAlongSeam(const SeamFlowGrid& g, std::vector<float>& share);

} // namespace insv
