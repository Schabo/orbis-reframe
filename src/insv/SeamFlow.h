#pragma once

// The new stitcher's seam flow: near things crossing the seam joined up by
// optical flow between the two lenses, the rest left at the template's
// geometry.
//
// Both lenses are unwrapped over the overlap round the seam circle into a
// strip each (SeamFlowStrips: azimuth phi along the seam across, elevation e
// off it down, at the template's geometry and capture-time correction, so
// the strips show what the stitch would sample). The flow between them
// (estimated outside this module -- stitch::EstimateSeamFlow) says where
// lens 0's content sits in lens 1 (F01) and lens 1's in lens 0 (F10).
// SeamFlowWindow averages it over the last few frames so the warp holds
// still; a frame's field depends only on those frames, so a seek that
// measures them first stitches the frame as playback into it does.
//
// The warp (FisheyeStitcher::seamFlowRay, mirrored in
// fisheye_stitch_common.glsl): one share b(e) across the seam -- 0 at
// +reach (lens 0's side), 1/2 on the seam, 1 at -reach, flat at both ends
// (SeamFlowShare). Lens 0 is sampled at (phi, e) + b F10, lens 1 at
// (phi, e) + (1 - b) F01: a thing lens 0 shows at x0 and lens 1 at x1 is
// shown at x0 + b (x1 - x0) by both, so each lens keeps its own geometry
// where it is shown alone and the two meet halfway on the seam. Past the
// strip (the reach can be wider than what both lenses see) the flow is the
// strip's outermost row's, faded out by the share.

#include "FisheyeProjection.h"
#include "Image.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace insv {

// The strips' resolution: as good as 10 px/deg in the measurements, and
// the flow on them half the time.
constexpr double kSeamFlowPxDeg = 7.0;
constexpr double kSeamFlowReachDeg = 8.0;  // the warp's reach either side of the seam
constexpr double kSeamFlowStripMaxDeg = 11.0; // the strips' half-height at most (reach + 3)
// Frames averaged (SeamFlowWindow): this one and those before, alike. As still
// as a longer window or smoothing in the measurements, and a seek has only
// the frames before it to measure.
constexpr int kSeamFlowFrames = 5;

// The strips' grid. Column x is azimuth phi = (x + 0.5) * 2 pi / cols, row
// y elevation e = stripRad - y * 2 stripRad / (rows - 1) (row 0 on lens 0's
// side). Flow values are radians of (phi, e).
struct SeamFlowGrid {
    int cols = 0;
    int rows = 0;
    float stripRad = 0; // the strips reach +-stripRad off the seam circle
    float reachRad = 0; // the warp's reach (SeamFlowShare)
    bool valid() const { return cols > 0 && rows > 1 && stripRad > 0 && reachRad > 0; }
    double phiOf(double x) const;
    double eOf(double y) const;
    double xOf(double phi) const; // inverse of phiOf, phi in [0, 2 pi)
    double yOf(double e) const;   // inverse of eOf
    bool operator==(const SeamFlowGrid& o) const
    {
        return cols == o.cols && rows == o.rows && stripRad == o.stripRad && reachRad == o.reachRad;
    }
};

// The grid for a configured stitcher: the strips reach as far out as both
// lenses still see the whole seam circle (some edge weight left), at most
// kSeamFlowStripMaxDeg. Invalid if they share less than a degree.
SeamFlowGrid SeamFlowGridFor(const FisheyeStitcher& st);

// The share b(e) of the flow a lens is moved by (lens 0 b, lens 1 1 - b):
// 0 at e >= reach, 1/2 at e = 0, 1 at e <= -reach.
double SeamFlowShare(double e, double reach);

// Both lenses' strips, grey (BT.601 luma of the 0..255 samples), rows x
// cols each, row-major; 0 where a lens has no sample.
struct SeamFlowStrips {
    SeamFlowGrid grid;
    std::vector<uint8_t> grey[2];
};

// The strips of a CPU-decoded pair at the stitcher's geometry (no
// capture-time correction: the CPU stitch has none) -- the GPU's strip pass
// on the CPU.
void MeasureSeamFlowStrips(const FisheyeStitcher& st, const SeamFlowGrid& grid, const Image& lens0,
                           const Image& lens1, SeamFlowStrips& out);

// Where a strip has no texture, any shift matches: clipped sky, the
// near-white veil the sun lays over the scene. The flow there is
// arbitrary, changes from frame to frame and, because a featureless patch
// fits perfectly wherever it is put, wins over its neighbours as the flow
// is spread across the strip -- the warp then drags the textured scene
// beside it round. So before the flow, both strips get the same fixed
// pattern (SeamFlowAnchorNoise, a function of the strip position alone)
// added where they are bright and have little texture of their own: there
// the two then match only unshifted, and the flow stays the template's,
// the same every frame. Texture is the RMS of the strip less its box mean,
// brightness the box mean, both over a box of +-kSeamFlowAnchorBoxPx (x
// wrapping round, rows past the ends repeating them); the pattern is full
// below kSeamFlowAnchorTexLo grey levels of texture and gone from
// kSeamFlowAnchorTexHi, gone below kSeamFlowAnchorBrightLo and full from
// kSeamFlowAnchorBrightHi. Only bright areas: darker flat ones (a road, a
// shaded wall) lie next to near things whose shift the flow has to find,
// and pinning them held those back. Mirrored in seam_flow.comp's anchor
// passes.
constexpr int kSeamFlowAnchorBoxPx = 8;
constexpr double kSeamFlowAnchorTexLo = 5;
constexpr double kSeamFlowAnchorTexHi = 20;
constexpr double kSeamFlowAnchorBrightLo = 140;
constexpr double kSeamFlowAnchorBrightHi = 180;
constexpr double kSeamFlowAnchorAmp = 32; // the pattern's RMS, grey levels

// The pattern at strip pixel (x, y): value noise over 4, 8 and 16 pixel
// cells from an integer hash, RMS about 1.
float SeamFlowAnchorNoise(int x, int y);

// `in`'s strips with the pattern added where they are bright and have no
// texture, rounded to 8 bits.
void AnchorSeamFlowStrips(const SeamFlowStrips& in, SeamFlowStrips& out);

// A flow field on the grid: per cell four floats -- F10 (dphi, de), then F01
// (dphi, de), radians. Empty = no flow (the template).
struct SeamFlowField {
    SeamFlowGrid grid;
    std::vector<float> v; // rows * cols * 4
    bool empty() const { return v.empty(); }
    // Bilinear at strip position (x, y): x wraps round, y clamps to the
    // strip. `out` F10 (dphi, de), F01 (dphi, de).
    void at(double x, double y, float out[4]) const;
};

// The last kSeamFlowFrames frames' fields, averaged.
// Frames come in play order; a gap (a seek) is the caller's to reset().
class SeamFlowWindow {
public:
    void reset() { frames_.clear(); }
    void add(SeamFlowField f);
    // The weighted mean of the frames held into `out` (empty with none).
    void solve(SeamFlowField& out) const;
    int size() const { return static_cast<int>(frames_.size()); }

private:
    std::deque<SeamFlowField> frames_;
};

} // namespace insv
