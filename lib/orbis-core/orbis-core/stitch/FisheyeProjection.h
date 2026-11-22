// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <orbis-core/stitch/BlurQuads.h>
#include <orbis-core/Image.h>
#include <orbis-core/stitch/LensColour.h>
#include <orbis-core/stitch/LensModel.h>
#include <orbis-core/stitch/LensProfile.h>
#include <orbis-core/stitch/SeamCorrection.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

// CPU fisheye -> equirectangular stitch (orbis-core).
//
// For each equirect texel: reconstruct the world ray, rotate each lens's ray into its
// own frame, project it through the lens's MEI calibration, sample, and
// feather-blend the two lenses across the overlap band, their colours matched
// by the clip's lens colour (orbiscore::LensColour). With StitchParams::depthSeam each lens is sampled from its own
// centre at a per-azimuth stitch distance (orbiscore::SeamDepth), so near content on
// the seam lines up; no dense optical-flow warp beyond that.
//
// World convention matches src/render/shaders/equirect.frag and
// src/export/ReframeRenderer: +X right, +Y up, -Z forward; equirect
// longitude = atan2(x, -z), latitude = asin(y).
//
// Dependency-free (no Qt / FFmpeg / Vulkan) -- part of the `orbiscore`
// static lib. The .insv demux that produces the input frames is the separate
// `orbiscore_media` lib (it needs FFmpeg); see FisheyeDemux.h.

namespace orbiscore {

struct SeamFlowField; // SeamFlow.h

// --- small linear algebra (double; public so tests can exercise it) --------

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

struct Mat3 {
    // row-major; identity by default
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

Vec3 normalize(const Vec3& v);
Vec3 apply(const Mat3& a, const Vec3& v); // a * v
Mat3 mul(const Mat3& a, const Mat3& b);   // a * b
Mat3 transpose(const Mat3& a);
Mat3 rotX(double rad);
Mat3 rotY(double rad);
Mat3 rotZ(double rad);

// Stitch parameters. The lens geometry itself comes from the calibration and
// the lens poses; these are the seam/blend choices.
struct StitchParams {
    // Per-lens cutoff / feather centre, and the feather half-width around it.
    // The two lenses face opposite ways, so 90 splits the sphere evenly; the
    // band is further capped by the lens field of view (fovDeg) so neither lens
    // is sampled past its usable edge.
    double seamDeg = 90.0;
    double featherDeg = 8.0;

    // Usable lens field of view, degrees: 200 for the bare X5 lens, 195 with
    // lens guards, 190 in a dive case (0x101 field 145). Sets the circle-edge
    // reference angle (fovDeg/2) and caps the seam band at
    // fovDeg/2 - seamDeg - kSeamMarginDeg.
    double fovDeg = 200.0;
    static constexpr double kSeamMarginDeg = 2.5;

    // Accessory lens-profile correction (lens guards, dive cases). The
    // trailer's MEI calibration describes the bare lens. An accessory bends the
    // field so each angle lands further out on the sensor -- ~2.3 % at the seam
    // for the X5 lens guards, which is a ~4 deg ghost across the seam if
    // ignored. When `accessory` is set, the MEI image radius is scaled by
    // accessoryPoly(theta) / barePoly(theta), both theta-in-degrees -> mm
    // polynomials from 0x101 field 145 (docs/formats/insta360-x5.md §5.4).
    bool accessory = false;
    double barePoly[6] = {0, 0, 0, 0, 0, 0};
    double accessoryPoly[6] = {0, 0, 0, 0, 0, 0};

    // Measured correction to the lens calibration: the seam alignment
    // fine-tune (orbiscore::EstimateSeamAlignment).
    SeamCorrection seam;
    // The correction the camera model's files need on top of their
    // calibration (camera::ClipMetadata::lensCorrection), in `seam`'s terms
    // and applied under it: turns and centres add, scales multiply. None =
    // the calibration as it is.
    SeamCorrection modelCorrection;

    // Depth-aware seam. The two lenses sit `baselineM` apart along their common
    // axis (the calibration's lens-1 tz, ~32 mm on the X5), so an object at
    // distance D on the seam is seen ~baselineM/D rad apart by the two lenses.
    // With `depthSeam` each lens is projected from its own centre at a
    // per-azimuth stitch distance (the table passed to renderEquirect /
    // StitchGpuParams::seamQ, solved per frame by orbiscore::SeamWindow),
    // fading back to infinity over `depthRampDeg` beyond the blend band. Off =
    // both lenses at one point.
    bool depthSeam = false;
    // 0 = from the calibration (|lens-1 tz|); negative swaps which lens is
    // in front (a diagnostic for the sign, tools/orbis_stitch --baseline).
    double baselineM = 0.0;
    double depthRampDeg = 20.0;

    // Circle-edge cutoff: a lens contributes fully while its sample radius
    // (normalised to 1 at fovDeg/2, see radiusForTheta) <= edgeStartFrac,
    // then feathers to 0 by edgeEndFrac. Stops the wide-angle rays near the poles from sampling the black/vignette ring
    // outside the imaged circle (the "pole smear"). The other lens still covers
    // that direction (combined FOV > 360 on the X5's ~200 deg lenses).
    double edgeStartFrac = 0.90;
    double edgeEndFrac = 1.00;

    // The clip's lens colour (orbiscore::FitLensColour): each lens's gain per
    // channel, so the seam has no step in brightness or colour. All 0 = as
    // recorded.
    LensColour lensColour;

    // Each lens's pose in the camera (camera::Lens::pose), which the
    // camera's reader knows; the default is no camera's (both lenses facing
    // the same way), so a caller that forgets it gets no seam rather than a
    // nearly right one.
    LensPose lensPose[2];
    // The lenses' model (camera::Lens::model; both have the same one), and
    // for the theta-polynomial each lens's fifth term (camera::Lens::k5).
    LensModel lensModel = LensModel::Mei;
    double thetaK5[2] = {0.0, 0.0};
    // Where each lens sees its own camera (camera::Lens::mask); none by
    // default.
    LensMask lensMask[2];

    // Global equirect orientation applied to the sampling ray (Ry(yaw) Rx(pitch)
    // Rz(roll)): how the camera holds its lenses, so the picture is level and
    // faces the camera's "forward". A clip's comes from its camera
    // (camera::ClipMetadata::mountYawDeg ...); the default is Insta360's, 180 =
    // Insta360 forward vs our lens 0.
    double outYawDeg = 180.0;
    double outPitchDeg = 0.0;
    double outRollDeg = 0.0;
};

// Sets `params` up for a lens accessory (a lens guard, a dive case): the
// usable FOV, and the accessory's radial scale against the bare lens's
// profile. `accessory` null = the bare lens. Returns false (and leaves the
// bare-lens setup) when a profile it needs is missing or unusable.
bool ApplyLensProfile(StitchParams& params, const LensProfile* bare,
                      const LensProfile* accessory);

// Built per-lens projection data, in decoded per-stream pixels.
struct LensProjection {
    Mat3 worldToLens; // R_lens^T

    // The lens model (StitchParams::lensModel) with the calibration's
    // coefficients (orbiscore::LensCalibExt). project() maps the lens-frame ray d
    // to the calibration's image axes (u right, v down -- our lens frame has
    // both flipped). MEI:
    //   x = -d.x/(d.z+xi), y = -d.y/(d.z+xi),  r^2 = x^2+y^2
    //   radial = 1 + k1 r^2 + k2 r^4 + k3 r^6 + k4 r^8
    // The theta-polynomial, theta the off-axis angle:
    //   theta_d = theta (1 + k1 theta^2 + k2 theta^4 + ... + k5 theta^10)
    //   x = -d.x theta_d/|d.xy|, y = -d.y theta_d/|d.xy|,  r^2 = theta_d^2
    //   radial = 1, s1..s4 = 0
    // Then for both:
    //   xd = x radial + 2 p1 x y + p2 (r^2 + 2x^2) + s1 r^2 + s2 r^4
    //   yd = y radial + p1 (r^2 + 2y^2) + 2 p2 x y + s3 r^2 + s4 r^4
    //   u = cx + fx*xd*acc(theta),  v = cy + fy*yd*acc(theta)
    // acc() is the accessory scale (StitchParams::accessory, 1 without).
    // The tangential/thin-prism terms move the rim by up to ~9 px; including
    // them in these axes measurably improves the seam match.
    LensModel model = LensModel::Mei;
    double cx = 0, cy = 0;
    double xi = 0, fx = 0, fy = 0;
    double k[4] = {0, 0, 0, 0};
    double k5 = 0; // the theta-polynomial's theta^10 term
    double p[2] = {0, 0};       // tangential
    double s[4] = {0, 0, 0, 0}; // thin-prism
};

// Depth-aware seam table: the seam circle is split into this many
// azimuth bins, each with its own inverse stitch distance q (1/m; 0 =
// infinity). Must match SEAM_BINS in fisheye_stitch_common.glsl.
constexpr int kSeamDepthBins = 128;

// The depth-aware seam's blend per azimuth bin (orbiscore::SeamWindow): where
// across the band the two lenses meet and how wide the fade there is. Narrow
// where the lenses agree at the meeting line, today's full-band cross-fade
// where they don't. A fade of 0 (no frame measured yet) is the plain
// cross-fade of StitchParams::featherDeg.
//
// And this frame's colour residual: what is left of the lenses' colour
// difference after the clip's lens colour (StitchParams::lensColour), added
// to its log ratio (orbiscore::LensColourLog); per azimuth bin a local correction
// on top, and the veiling light one lens adds there (orbiscore::LensVeilPerBin).
struct SeamBlend {
    // Elevation of the fade's centre off the seam circle, radians, towards
    // lens 0 (the seam frame, FisheyeStitcher::seamDir).
    float cut[kSeamDepthBins] = {};
    // Half-width of the fade, radians.
    float fade[kSeamDepthBins] = {};
    // log(lens 0 / lens 1), R G B.
    float colour[3] = {};
    // And per azimuth bin what is left of it after that, near the seam
    // (orbiscore::kLensColourLocalMaxLog), R G B.
    float local[kSeamDepthBins][3] = {};
    // Veiling light per azimuth bin, linear, R G B: lens 0 less lens 1 (> 0
    // lens 0 has it, < 0 lens 1), taken out of that lens.
    float veil[kSeamDepthBins][3] = {};
};

// Everything a GPU port of renderEquirect() needs, as plain floats -- one
// source of truth for the geometry so the compute shader stays a faithful
// mirror of the CPU path. Populated by FisheyeStitcher::gpuParams(). All
// angles in radians; matrices row-major 3x3.
struct StitchGpuParams {
    float worldToLens[2][9];
    float outRot[9];
    // The lens model (see LensProjection): thetaPoly 1 = the
    // theta-polynomial, its fifth terms in k5; else MEI.
    int thetaPoly = 0;
    float k5[2] = {0, 0};
    float cx[2] = {0, 0};
    float cy[2] = {0, 0};
    float xi[2] = {0, 0};
    float fx[2] = {0, 0};
    float fy[2] = {0, 0};
    float kdist[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    float pdist[2][2] = {{0, 0}, {0, 0}};             // tangential p1, p2
    float sdist[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}}; // thin-prism s1..s4
    // Accessory scale acc(theta) = accessoryPoly(deg) / barePoly(deg); off
    // when accessory == 0. See StitchParams::accessory.
    int accessory = 0;
    float barePoly[6] = {0, 0, 0, 0, 0, 0};
    float accessoryPoly[6] = {0, 0, 0, 0, 0, 0};
    float thetaRef = 0; // circle-edge angle (fovDeg/2), radians
    float seamRad = 0;
    float bandRad = 0;
    float thetaMaxRad = 0;
    float edgeStartFrac = 0.90f;
    float edgeEndFrac = 1.00f;
    // The clip's lens colour (StitchParams::lensColour): the log ratio lens 0
    // / lens 1 on the seam circle, R G B, and its slope per radian of
    // elevation; the per-frame residual (seamBlend.colour) is added to the
    // first by the packer.
    float lensColour[3] = {0, 0, 0};
    float lensColourSlope[3] = {0, 0, 0};
    int fisheyeW = 0;
    int fisheyeH = 0;
    // Where each lens's fisheyeW x fisheyeH picture is in the image the GPU
    // samples, x, y, w, h as fractions of it (LensSource::rect): the whole
    // image, or a half where one decoded frame holds both lenses (a camera's
    // proxy). Sampling stays inside the lens's own part.
    float lensRect[2][4] = {{0, 0, 1, 1}, {0, 0, 1, 1}};
    // Per-frame capture-time correction (GPU stitch only; rsActive 0 = off).
    // Lens i's fisheye row v was captured at the frame time + lens offset +
    // (v/fisheyeH - 0.5) * that lens's readout (stitch::CaptureTiming); the
    // shader rotates the sampled ray back by how far the camera turned by
    // then. rsRot[i][k] is that rotation (rotation vector, rad, equirect /
    // sphere frame) at row fraction k/(kRsTaps-1), interpolated per row -- a
    // table rather than one rate because the stick mount's ~11 Hz swing
    // changes direction within a readout. rsUseColumns scans along u.
    static constexpr int kRsTaps = 7;
    float rsRot[2][kRsTaps][3] = {};
    int rsActive = 0;
    int rsUseColumns = 0;
    // Depth-aware seam (StitchParams::depthSeam). The seam frame: `seamAxis`
    // is the lenses' common axis (lens 0's way), `seamU`/`seamV` span the
    // seam plane (azimuth phi = atan2(v, u)). Lens 0 sits at
    // +halfBaseline * seamAxis, lens 1 at -halfBaseline * seamAxis (metres).
    // Per frame: seamQ[b] is the inverse stitch distance of azimuth bin b
    // (bin centres at (b + 0.5) * 2pi / kSeamDepthBins).
    int depthActive = 0;
    float seamAxis[3] = {0, 0, 1};
    float seamU[3] = {1, 0, 0};
    float seamV[3] = {0, 1, 0};
    float halfBaseline = 0;
    float depthRampRad = 0;
    float seamQ[kSeamDepthBins] = {};
    // The lens masks (StitchParams::lensMask) on the seam frame, per azimuth
    // bin: lens 0 sees the scene above elevation seamClear[b][0], lens 1
    // below seamClear[b][1] (radians, towards lens 0), each the bin's most
    // cautious over its width (FisheyeStitcher::lensClear). maskActive 0 =
    // no mask, the table unused.
    int maskActive = 0;
    float seamClear[kSeamDepthBins][2] = {};
    // Per frame: the blend across the band (all 0 = the plain cross-fade).
    SeamBlend seamBlend;
    // Per frame: the new stitcher's seam colour blend (orbiscore::SeamColourBlend)
    // -- per azimuth bin log(lens 0 / lens 1) R G B, half to each lens,
    // faded across the seam by orbiscore::SeamColourFade. 0 = off.
    int seamColourActive = 0;
    float seamColour[kSeamDepthBins][3] = {};
    // ... and the veiling light it takes out (SeamColourWindow::solve's
    // `veil`: per bin R G B, linear, lens 0 less lens 1), with it; it also
    // sets the fade between the lenses (SeamVeilFade).
    float seamVeil[kSeamDepthBins][3] = {};
    // The new stitcher's seam flow (SeamFlow.h). Its grid (SeamFlowGrid;
    // cols 0 = no seam flow), which the GPU's strip pass measures on, and
    // per frame the field; null or empty = none. The GPU stitch uploads the
    // field as a texture when it changes (and hands it on with the lens
    // frames), the CPU stitch reads it.
    int seamFlowCols = 0;
    int seamFlowRows = 0;
    float seamFlowStripRad = 0;
    float seamFlowReachRad = 0;
    std::shared_ptr<const SeamFlowField> seamFlow;
    // Instead of `seamFlow`: the GPU stitcher's own field, the one its seam
    // flow window put into the flow texture (render::VulkanFisheyeStitcher::
    // measureSeamFlowVk), on the grid above. GPU stitch only.
    bool seamFlowGpu = false;
    // Privacy blur (BlurQuads.h): regions blurred in this frame, equirect
    // (sphere) frame, prepared. Per frame, like the RS taps. At most
    // kMaxBlurQuads: the GPU stitch refuses a frame with more.
    std::vector<BlurQuadFrame> blur;
};

class FisheyeStitcher {
public:
    // `cal` must carry the extended MEI calibration for exactly 2 lenses;
    // `streamW/streamH` are the decoded per-lens fisheye frame dimensions
    // (3840x3840 on the X5). Returns false otherwise. Cheap; safe to call again on a params change.
    bool configure(const CameraCalibration& cal, int streamW, int streamH,
                   const StitchParams& params);

    bool ready() const { return m_ready; }

    // Reproject `lens0`/`lens1` into `out`, which the caller has already
    // alloc()'d to the desired 2:1 equirect size. No-op (leaves `out` as given)
    // when !ready(). Multithreaded over scanlines. `seamQ` (kSeamDepthBins
    // values, nullptr = all 0) is the depth-aware seam's per-azimuth inverse
    // stitch distance and `blend` its blend across the band (nullptr = the
    // plain cross-fade); both used only with StitchParams::depthSeam.
    // `blur` (`blurCount` prepared quads, BlurQuads.h) are blurred.
    // `seamColour` (StitchGpuParams::seamColour's layout, null = none) is the
    // new stitcher's seam colour blend (seamColourLog) and `seamVeil`
    // (StitchGpuParams::seamVeil's, null = none) the veiling light it takes
    // out (seamVeil); `flow` (null or empty = none) its seam flow
    // (seamFlowRay). With both and the flow's fade (SeamFlowField::fade) the
    // lenses meet in its narrow fade, widened where the field's fade or the
    // veiling light (SeamVeilFade) says; else the plain cross-fade.
    void renderEquirect(const Image& lens0, const Image& lens1, Image& out,
                        const float* seamQ = nullptr, const SeamBlend* blend = nullptr,
                        const BlurQuadFrame* blur = nullptr, int blurCount = 0,
                        const float* seamColour = nullptr,
                        const SeamFlowField* flow = nullptr,
                        const float* seamVeil = nullptr) const;

    // Convenience: allocate a `width x width/2` equirect and render into it.
    Image renderEquirect(const Image& lens0, const Image& lens1, int width,
                         const float* seamQ = nullptr, const SeamBlend* blend = nullptr) const;

    // Flattened geometry for a GPU port (see StitchGpuParams). Valid after a
    // successful configure().
    StitchGpuParams gpuParams() const;

    // --- introspection, for tests / debugging -----------------------------
    const LensProjection& lensProjection(int i) const { return m_lens[i]; }
    const Mat3& outRotation() const { return m_outRot; }

    // world dir -> (in front of the lens?, pixel u,v, off-axis angle theta).
    bool project(int lens, const Vec3& world, double& u, double& v, double& theta) const;

    // Image radius for an off-axis angle (radians), normalised to 1 at the
    // circle edge (fovDeg/2). Drives the edge feather.
    double radiusForTheta(double thetaRad) const;

    // Accessory radial scale at an off-axis angle (radians); 1 without one.
    double accessoryScale(double thetaRad) const;

    // --- depth-aware seam (StitchParams::depthSeam) -----------------------
    bool depthSeam() const { return m_params.depthSeam; }
    // The seam frame (see StitchGpuParams::seamAxis) and lens i's centre.
    const Vec3& seamAxis() const { return m_seamAxis; }
    const Vec3& seamU() const { return m_seamU; }
    const Vec3& seamV() const { return m_seamV; }
    Vec3 lensCentre(int lens) const;
    // The distance between the lenses' centres, metres.
    double baselineM() const { return 2.0 * std::fabs(m_halfBaseline); }
    // Direction on the seam frame at azimuth phi, elevation e (towards lens 0).
    Vec3 seamDir(double phi, double e) const;
    // The inverse stitch distance for world dir `dir` from the per-bin table:
    // linear in azimuth, faded to 0 past the blend band. 0 when off / null.
    double seamQAt(const Vec3& dir, const float* seamQ) const;
    // Where `dir` is on the seam: elevation e off the seam circle (radians,
    // towards lens 0) and its position among the azimuth bins -- bins i0 and
    // i1 = i0 + 1 (mod kSeamDepthBins) around it, t its fraction from i0 to
    // i1 (bin centres at (b + 0.5) * 2pi / kSeamDepthBins).
    void seamPosition(const Vec3& dir, double& e, int& i0, int& i1, double& t) const;
    // The direction lens i sees the point at distance 1/q along `dir` in.
    Vec3 lensRay(int lens, const Vec3& dir, double q) const;
    // The direction lens i is sampled in for `dir` with the seam flow `f`
    // (SeamFlow.h): `dir` moved along the seam frame by lens i's share of
    // the flow there; `dir` itself past the flow's reach.
    Vec3 seamFlowRay(int lens, const Vec3& dir, const SeamFlowField& f) const;
    // Blend weight of lens i for world dir `dir` (seam feather only, no
    // circle edge): from dir's own off-axis angle, whatever the depth.
    double seamBlendWeight(int lens, const Vec3& dir) const;
    // Both lenses' blend weights for `dir` with the per-bin `blend` (seam
    // feather only; the depth-aware seam's cut, or the new stitcher's
    // SeamVeilFade): the fade from dir's elevation off the seam circle. The
    // plain cross-fade (seamBlendWeight) when `blend` is null or not
    // measured yet. With `fadeField`'s fade (the new stitcher's, SeamFade.h)
    // the half-width is the wider of the bin's and the field's there.
    void seamBlendWeights(const Vec3& dir, const SeamBlend* blend, double w[2],
                          const SeamFlowField* fadeField = nullptr) const;
    // The field's fade share (SeamFlowField::fade) at dir's azimuth,
    // linear between columns; 1 (the whole band) without one.
    double seamFadeShare(const Vec3& dir, const SeamFlowField& f) const;
    // Each lens's log gain per channel for `dir` from the clip's lens colour
    // and the frame's residual and local correction (`blend`, null = none;
    // the local one in the band, faded out over kLensColourLocalRampDeg past
    // it): lens 0 -L / 2, lens 1 L / 2 (orbiscore::LensColourLog). Applied with
    // LensColourGain, after `veil`: each lens's veiling light there
    // (linear, >= 0, faded out as the local correction), taken out with
    // LensVeilRemove.
    void lensLogGains(const Vec3& dir, const SeamBlend* blend, double lg[2][3],
                      double veil[2][3]) const;
    // The seam colour blend's log ratio lens 0 / lens 1 for `dir` from the
    // per-bin `table` (StitchGpuParams::seamColour's layout): interpolated
    // between bins, faded across the seam (orbiscore::SeamColourFade). Lens 0
    // takes -L / 2, lens 1 L / 2, as lensLogGains.
    void seamColourLog(const Vec3& dir, const float* table, double L[3]) const;
    // The new stitcher's veiling light for `dir` from the per-bin `table`
    // (StitchGpuParams::seamVeil's layout) into each lens's `veil` (linear,
    // >= 0; taken out with LensVeilRemove): interpolated between bins, in the
    // band and faded out over kLensColourLocalRampDeg past it, as
    // lensLogGains has the old stitcher's.
    void seamVeil(const Vec3& dir, const float* table, double veil[2][3]) const;
    // Circle-edge weight for an off-axis angle (radians) actually sampled.
    double edgeWeight(double thetaRad) const;

    // --- lens masks (StitchParams::lensMask) -------------------------------
    bool hasLensMask() const { return m_hasMask; }
    // Whether lens i sees the scene at `dir`, not its own camera: from the
    // per-bin table (StitchGpuParams::seamClear), the more cautious of the
    // two bins round dir. Always true without a mask.
    bool lensClear(int lens, const Vec3& dir) const;
    // The two lenses' feather `w` (seamBlendWeights) kept off the masks: as
    // dir nears where a lens's mask starts, that lens's weight goes over to
    // the other, by a smoothstep over the nearer of kLensMaskRampDeg and half
    // the clear stretch both share; where they share none, each lens up to
    // the middle of it. Untouched (to the bit) where neither mask is near.
    void clearFade(const Vec3& dir, double w[2]) const;
    static constexpr double kLensMaskRampDeg = 2.0;

private:
    bool m_ready = false;
    bool m_accessory = false; // accessory radial scale active
    StitchParams m_params;
    LensProjection m_lens[2];
    Mat3 m_outRot;
    int m_streamW = 0;
    int m_streamH = 0;
    double m_thetaRefRad = 0;
    double m_thetaSeamRad = 0;
    double m_bandRad = 0;
    double m_thetaMaxRad = 0;
    Vec3 m_seamAxis{0, 0, 1};
    Vec3 m_seamU{1, 0, 0};
    Vec3 m_seamV{0, 1, 0};
    double m_halfBaseline = 0;
    bool m_hasMask = false;
    float m_clear[kSeamDepthBins][2] = {};
    void buildMaskTable(); // m_clear from m_params.lensMask, after the seam frame
};

} // namespace orbiscore
