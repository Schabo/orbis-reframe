// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

// Blur regions in the stitch (privacy blur): a region is a quad on the sphere
// -- four corner directions -- and everything the stitch renders inside it is
// replaced by a smooth blur of it, so a number plate in it can't be read in
// any view or export made from that stitch. Done where the lenses are sampled
// (fisheye_stitch_common.glsl blurredLenses(), FisheyeStitcher::renderEquirect
// on the CPU), so every path -- the equirect, the fused view, the viewport --
// carries it.
//
// The quad is flattened onto the tangent plane at its centroid (gnomonic,
// where it stays a straight-edged quad) and parametrised bilinearly: (s, t)
// in [0, 1]^2, s along TL -> TR, t along TL -> BL, and cut into a grid of
// cells. Each cell's colour is the mean of kBlurCellTaps^2 taps spread evenly
// over it -- a box filter, so a cell's colour changes gradually as the quad
// moves over the picture -- worked out once per frame (the stitch's pass 5,
// FisheyeStitcher::renderEquirect's first step). A point in the quad takes
// the bilinear mix of the four cell colours round it (cell centres at
// ((i + 0.5) / cells)), so the picture has no cell edges that jump when the
// detected corners jitter; nothing finer than a cell survives either way.
// The outer kBlurFeather of (s, t) fades into the picture.
//
// Where regions overlap (a hand-drawn region over a plate the scan found,
// two scanned plates close together), adding a region never lets more of the
// picture through: of the picture, prod(1 - w) is left, w each region's fade
// weight there, so a point in any region's interior is fully blurred in
// whatever order the regions come. The blur colour there is the regions'
// colours averaged by w; a lone region is its own colour, as without overlap.
//
// Dependency-free, like the rest of orbis-core.

#include <vector>

namespace orbiscore {

// Per stitch call: the GPU stitch's region buffer and cell table hold this
// many. A frame with more isn't stitched at all -- never with some of its
// regions left out -- so the source refuses it (stitch::NativeStitchSource).
// A busy car park has a few dozen readable plates in view at most.
constexpr int kMaxBlurQuads = 256;
// Cells across the quad's shorter side; the longer side gets as many as keep
// the cells roughly square.
constexpr int kBlurCellsShort = 5;
constexpr int kBlurCellsMax = 40;
// A quad's cells, at most (the short side is kBlurCellsShort): the row of
// the cell table each quad has (cell (i, j) at j * cells[0] + i).
constexpr int kBlurCellsPerQuad = kBlurCellsShort * kBlurCellsMax;
// Taps per cell side (kBlurCellTaps^2 samples average into one cell colour).
// 16 keeps the taps within ~2 lens pixels of each other on a close plate's
// ~30 px cells, so the mean is a box filter, not a few point samples.
constexpr int kBlurCellTaps = 16;
// Width of the fade at the quad's edge, in (s, t) units.
constexpr float kBlurFeather = 0.08f;

// A region: corner directions TL, TR, BR, BL in the equirect (sphere) frame,
// unit length, all within ~80 deg of each other.
struct BlurQuad {
    float corner[4][3] = {};
};

// The form the stitch tests against (prepareBlurQuad): the centroid
// direction `c` and the tangent basis `r`, `u` there; the corners on that
// plane divided by `scale` (so the inverse-bilinear solve is well
// conditioned whatever the quad's angular size); the mosaic cell counts.
struct BlurQuadFrame {
    float c[3] = {0, 0, -1};
    float r[3] = {1, 0, 0};
    float u[3] = {0, 1, 0};
    float cosRadius = 1.0f; // dot(dir, c) below this is outside, a cheap reject
    float scale = 1.0f;     // tangent-plane units per corner unit
    float p[4][2] = {};     // corners on the tangent plane / scale
    float cells[2] = {1, 1}; // along s, t
};

// False when the quad is degenerate (a corner > 80 deg off the centroid, or
// zero area); `out` is then unusable.
bool prepareBlurQuad(const BlurQuad& q, BlurQuadFrame& out);

// A frame's quads prepared into `out`, in order; a degenerate one covers
// nothing and is left out. False when more than kMaxBlurQuads are left:
// `out` then has them all (its size says how many) and the frame must not be
// stitched with them -- nor with only some.
bool prepareBlurQuads(const std::vector<BlurQuad>& quads, std::vector<BlurQuadFrame>& out);

// Where `dir` (unit, same frame as the quad) falls in the quad: true with
// (s, t) in [0, 1]^2 when inside.
bool blurQuadST(const BlurQuadFrame& f, const double dir[3], double& s, double& t);

// The direction at (s, t) -- the bilinear point on the tangent plane, back on
// the sphere. Inverse of blurQuadST inside the quad.
void blurQuadDir(const BlurQuadFrame& f, double s, double t, double dir[3]);

// Blur weight at (s, t): 1 inside, ramping to 0 over kBlurFeather at the
// edges.
double blurQuadWeight(double s, double t);

// The quad's number of cells, cells[0] x cells[1] (<= kBlurCellsPerQuad).
int blurCellCount(const BlurQuadFrame& f);

// The direction of tap (i, j) (0 .. kBlurCellTaps-1 each) of cell `cell`
// (j * cells[0] + i order). The cell's colour is the mean of what the stitch
// renders at all kBlurCellTaps^2 of them.
void blurCellTapDir(const BlurQuadFrame& f, int cell, int i, int j, double dir[3]);

// The bilinear mix at (s, t): the four cells round it and their weights
// (summing to 1; at the quad's edge the nearest cells repeat).
void blurCellMix(const BlurQuadFrame& f, double s, double t, int cell[4], double w[4]);

} // namespace orbiscore
