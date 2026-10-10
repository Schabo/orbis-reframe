// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

// Mosaic regions in the stitch (privacy blur): a region is a quad on the sphere -- four corner directions -- and
// everything the stitch renders inside it is replaced by a coarse mosaic, so
// a number plate in it can't be read in any view or export made from that
// stitch. Done where the lenses are sampled (fisheye_stitch_common.glsl
// blurredLenses(), FisheyeStitcher::renderEquirect on the CPU), so every
// path -- the equirect, the fused view, the viewport -- carries it.
//
// The quad is flattened onto the tangent plane at its centroid (gnomonic,
// where it stays a straight-edged quad) and parametrised bilinearly: (s, t)
// in [0, 1]^2, s along TL -> TR, t along TL -> BL. A point's cell is
// floor((s, t) * cells); its colour is the mean of a few taps spread over the
// cell. Every pixel of a cell gets the same colour, so nothing finer than a
// cell survives. The outer kBlurFeather of (s, t) fades into the picture.
//
// Dependency-free, like the rest of src/insv.

namespace insv {

// Per stitch call. A frame with more is clamped (the caller warns).
constexpr int kMaxBlurQuads = 64;
// Cells across the quad's shorter side; the longer side gets as many as keep
// the cells roughly square.
constexpr int kBlurCellsShort = 5;
constexpr int kBlurCellsMax = 40;
// Taps per cell side (kBlurTaps^2 samples average into one cell colour).
constexpr int kBlurTaps = 3;
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

// Where `dir` (unit, same frame as the quad) falls in the quad: true with
// (s, t) in [0, 1]^2 when inside.
bool blurQuadST(const BlurQuadFrame& f, const double dir[3], double& s, double& t);

// The direction at (s, t) -- the bilinear point on the tangent plane, back on
// the sphere. Inverse of blurQuadST inside the quad.
void blurQuadDir(const BlurQuadFrame& f, double s, double t, double dir[3]);

// Mosaic weight at (s, t): 1 inside, ramping to 0 over kBlurFeather at the
// edges.
double blurQuadWeight(double s, double t);

// The direction of tap (i, j) (0 .. kBlurTaps-1 each) of the cell that holds
// (s, t). The cell's colour is the mean of what the stitch renders at all
// kBlurTaps^2 of them.
void blurTapDir(const BlurQuadFrame& f, double s, double t, int i, int j, double dir[3]);

} // namespace insv
