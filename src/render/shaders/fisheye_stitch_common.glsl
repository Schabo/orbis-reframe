// Shared projection core for the fisheye->equirect compute stitch. The
// includer must, BEFORE #include-ing this file, declare its two lens image
// bindings (1 and 2) and define (or, if it needs Params, declare):
//
//     vec3 sampleLens(int i, vec2 uv);   // returns RGB in [0, 255]
//
// so the RGB (fisheye_stitch.comp) and NV12/ycbcr (fisheye_stitch_nv12.comp)
// variants can share everything else. Kept a faithful mirror of the CPU
// reference insv::FisheyeStitcher::renderEquirect -- constants come from
// insv::FisheyeStitcher::gpuParams().
//
// Pass 1 (pc.pass==1) renders, each lens's colour matched by the clip's lens
// colour (insv::LensColour). Passes 2 and 3 (their own submission, before
// it) measure for the depth-aware seam: the
// cost volume (seamCostPass, one workgroup per azimuth bin and candidate
// distance, NCC of the two lenses over the bin's patch) and the cut
// statistics (seamCutPass, one workgroup per bin and row across the band).
// Pass 4 (likewise) unwraps both lenses over the overlap for the new
// stitcher's seam flow (seamStripPass, insv::MeasureSeamFlowStrips).
//
// The seam flow's field (insv::SeamFlowField, F10 in .xy, F01 in .zw) is the
// texture at SEAM_FLOW_BINDING (6 unless the includer says otherwise: the
// viewport has its lens planes at 1-4); used when P.seamFlow.x != 0.
//
// An includer that defines FISHEYE_STITCH_NO_MAIN (fisheye_view_nv12.comp,
// the fused fisheye -> view render) gets only the projection core and the
// per-direction helpers (eqTexelDir / lensWeights / blendLenses) and
// declares its own push constants and main(). One that also defines
// FISHEYE_STITCH_NO_BUFFERS (fisheye_view.frag, the viewport) gets no OutBuf
// / SeamBuf.

#define RS_TAPS 7 // == insv::StitchGpuParams::kRsTaps
#define SEAM_BINS 128 // == insv::kSeamDepthBins
#define SEAM_CANDS 40 // == insv::kSeamDepthCands
#define SEAM_PATCH 16 // == insv::kSeamPatchAz == insv::kSeamPatchEl
#define SEAM_ROWS 9 // == insv::kSeamCutRows
#define SEAM_CUT_AZ 16 // == insv::kSeamCutPatchAz (x insv::kSeamCutPatchEl = 64 samples)
#define SEAM_CUT_EL 4 // == insv::kSeamCutPatchEl
#define SEAM_CUT_EL_RAD 0.010471976 // == insv::kSeamCutPatchElDeg (0.6 deg)
#define SEAM_CUT_CLIP 250.0 // == insv::kSeamCutClip
#define SEAM_CUT_FADE_RAD 0.013089969 // == insv::kSeamCutFadeDeg (0.75 deg)
#define LENS_COLOUR_MAX_LOG 0.3 // == insv::kLensColourMaxLog
#define LENS_COLOUR_LOCAL_RAMP_RAD 0.17453293 // == insv::kLensColourLocalRampDeg (10 deg)
#define LENS_COLOUR_KNEE_LO 192.0 // == insv::kLensColourKneeLo
#define LENS_COLOUR_KNEE_HI 250.0 // == insv::kLensColourKneeHi
#define SEAM_COLOUR_FULL_RAD 0.20943951 // == insv::kSeamColourFullDeg (12 deg)
#define SEAM_COLOUR_END_RAD 0.52359878 // == insv::kSeamColourEndDeg (30 deg)
#define LENS_VEIL_GAMMA 2.2 // == insv::kLensVeilGamma
#define LENS_VEIL_KEEP 0.3 // == insv::kLensVeilKeep
#define MAX_BLUR 64 // == insv::kMaxBlurQuads
#define BLUR_TAPS 3 // == insv::kBlurTaps
#define BLUR_FEATHER 0.08 // == insv::kBlurFeather

layout(std140, binding = 0) uniform Params {
    vec4 w2l0[3];   // worldToLens[0] rows (.xyz)
    vec4 w2l1[3];   // worldToLens[1] rows
    vec4 outRot[3]; // global equirect orientation rows
    vec4 cxcy;      // cx0, cy0, cx1, cy1   (fisheye-pixel space)
    vec4 seam;      // seamRad, bandRad, thetaMaxRad, thetaRef (circle edge, rad)
    vec4 edge;      // edgeStartFrac, edgeEndFrac, seam colour blend on (0/1), -
    ivec4 dims;     // fisheyeW, fisheyeH, eqW, eqH
    // Extended MEI (Mei/unified) model. See insv::LensProjection / gpuParams().
    vec4 meiA;      // xi0, xi1, fx0, fx1
    vec4 meiB;      // fy0, fy1, -, accessory(as float)
    vec4 kd0;       // lens 0 radial distortion k1..k4
    vec4 kd1;       // lens 1 radial distortion k1..k4
    vec4 rs;        // capture timing: x 1 = on, y 1 = scan along u (columns)
    vec4 rsRot[2 * RS_TAPS]; // per lens: camera rotation to row time k (rotation vector, world frame), see projectRS
    vec4 pd0;       // lens 0 tangential p1, p2, thin-prism s1, s2
    vec4 pd1;       // lens 1 likewise
    vec4 sd;        // thin-prism s3, s4 of lens 0, then of lens 1
    vec4 accA;      // accessory poly c0..c3   (theta deg -> mm, 0x101 field 145)
    vec4 accB;      // accessory poly c4, c5, bare poly c0, c1
    vec4 accC;      // bare poly c2..c5
    // Depth-aware seam (insv::StitchGpuParams::seamAxis etc.).
    vec4 seamA;     // seam axis (lens 0's way) xyz, depth active (0/1)
    vec4 seamU;     // seam-plane basis u xyz, half baseline (m; lens 0 at +h * axis)
    vec4 seamV;     // seam-plane basis v xyz, ramp past the band (rad)
    vec4 seamC;     // cost pass: candidate q min, q max (1/m), patch elevation half-range (rad);
                    // cut pass: outermost row's elevation (rad, insv::SeamCutRowMax)
    vec4 seamQ[SEAM_BINS / 4]; // per azimuth bin: inverse stitch distance (1/m)
    vec4 seamCF[SEAM_BINS / 2]; // per azimuth bin: the fade's centre, half-width (rad; insv::SeamBlend), 2 bins per vec4
    // The lens colour (insv::LensColourLog): log(lens 0 / lens 1) R, G, B on
    // the seam circle, the frame's residual added; its slope per radian.
    vec4 lensCol;   // R, G, B, -
    vec4 lensColS;  // R, G, B, -
    vec4 lensLocal[SEAM_BINS]; // per azimuth bin: the local correction R, G, B, - (insv::SeamBlend::local)
    vec4 lensVeil[SEAM_BINS];  // per azimuth bin: the veiling light R, G, B, - (insv::SeamBlend::veil;
                               // the new stitcher's, StitchGpuParams::seamVeil, when edge.z != 0)
    // The new stitcher's seam colour blend (insv::StitchGpuParams::seamColour):
    // per azimuth bin log(lens 0 / lens 1) R, G, B, -; on when edge.z != 0.
    vec4 seamCol[SEAM_BINS];
    // The new stitcher's seam flow (insv::StitchGpuParams::seamFlow): on,
    // the strips' half-height and the warp's reach (rad), -; its grid's
    // columns and rows.
    vec4 seamFlow;
    ivec4 seamFlowN;
    // Privacy blur (insv::BlurQuadFrame, carried into the stitch world frame):
    // per quad 6 vec4: c.xyz + cells along s, r.xyz + cells along t, u.xyz +
    // cos radius, corners 0 / 1 (xy, xy), corners 2 / 3, (scale, -, -, -).
    vec4 blur0; // count, -, -, -
    vec4 blurQ[MAX_BLUR * 6];
    // The lenses' Y'CbCr -> R'G'B' (insv::YcbcrToRgbRows): component i is
    // dot(ycc[i].xyz, vec3(y, cb, cr)) + ycc[i].w on the planes' texel values.
    // For fisheye_view.frag, which samples the planes itself; the stitch's
    // ycbcr sampler converts on its own.
    vec4 ycc[3];
} P;

#ifndef SEAM_FLOW_BINDING
#define SEAM_FLOW_BINDING 6
#endif
layout(binding = SEAM_FLOW_BINDING) uniform sampler2D seamFlowTex;

#ifndef FISHEYE_STITCH_NO_BUFFERS
layout(std430, binding = 3) writeonly buffer OutBuf { uint px[]; };
// Pass 2 output: per (bin, candidate) = bin * SEAM_CANDS + k: (NCC, texture),
// as insv::SeamCosts. Pass 3's after it: per (bin, row) = bin * SEAM_ROWS + r,
// four vec4 -- (samples, zero-mean difference, texture, -), the unclipped
// samples per channel RGB, lens 0's mean RGB, lens 1's -- as
// insv::SeamCutStats::Cell.
layout(std430, binding = 5) writeonly buffer SeamBuf {
    vec2 seamCost[SEAM_BINS * SEAM_CANDS];
    vec4 seamCutStat[];
};
// Pass 4 output: both lenses' strips, grey 0..255, four columns per uint
// (lowest byte first), lens 0's rows then lens 1's (insv::SeamFlowStrips).
layout(std430, binding = 7) writeonly buffer StripBuf { uint seamStrip[]; };
#endif

#ifndef FISHEYE_STITCH_NO_MAIN
layout(push_constant) uniform PC { int pass; } pc;
#endif

const float PI = 3.14159265358979323846;

vec3 mul3(vec4 r0, vec4 r1, vec4 r2, vec3 v)
{
    return vec3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v));
}

// Rotationally-symmetric part of the MEI model: sampled radius (units of fx)
// vs. off-axis angle.  rho = sin/(cos+xi), then the radial polynomial.
float meiRadial(float theta, float xi, vec4 k)
{
    float denom = cos(theta) + xi;
    if (denom < 1e-6)
        return 1e9;
    float rho = sin(theta) / denom;
    float r2 = rho * rho, r4 = r2 * r2;
    return rho * (1.0 + k.x * r2 + k.y * r4 + k.z * r2 * r4 + k.w * r4 * r4);
}

// Accessory (lens guard / dive case) radial scale: accessoryPoly(deg) /
// barePoly(deg). 1 without an accessory. See insv::StitchParams::accessory.
float accessoryScale(float theta)
{
    if (P.meiB.w == 0.0)
        return 1.0;
    float t = max(degrees(theta), 1e-3);
    float pa = ((((P.accB.y * t + P.accB.x) * t + P.accA.w) * t + P.accA.z) * t + P.accA.y) * t + P.accA.x;
    float pb = ((((P.accC.w * t + P.accC.z) * t + P.accC.y) * t + P.accC.x) * t + P.accB.w) * t + P.accB.z;
    return (pb > 1e-9 && pa > 0.0) ? pa / pb : 1.0;
}

// Normalised so it is ~1 at thetaRef, the FOV edge (what circleEdgeWeight
// expects).
float radiusForTheta(float theta)
{
    float tref = max(P.seam.w, 1e-3);
    float ref = max(meiRadial(tref, P.meiA.x, P.kd0) * accessoryScale(tref), 1e-9);
    return meiRadial(theta, P.meiA.x, P.kd0) * accessoryScale(theta) / ref;
}

// world dir -> (u, v, theta); theta < 0 signals "reject".
vec3 project(int i, vec3 world)
{
    vec3 d;
    if (i == 0)
        d = normalize(mul3(P.w2l0[0], P.w2l0[1], P.w2l0[2], world));
    else
        d = normalize(mul3(P.w2l1[0], P.w2l1[1], P.w2l1[2], world));

    float theta = acos(clamp(d.z, -1.0, 1.0));
    if (theta > P.seam.z)
        return vec3(0.0, 0.0, -1.0);
    float cx = (i == 0) ? P.cxcy.x : P.cxcy.z;
    float cy = (i == 0) ? P.cxcy.y : P.cxcy.w;
    float xi = (i == 0) ? P.meiA.x : P.meiA.y;
    float fx = (i == 0) ? P.meiA.z : P.meiA.w;
    float fy = (i == 0) ? P.meiB.x : P.meiB.y;
    vec4 k = (i == 0) ? P.kd0 : P.kd1;
    vec4 pd = (i == 0) ? P.pd0 : P.pd1;
    vec2 s34 = (i == 0) ? P.sd.xy : P.sd.zw;
    float denom = d.z + xi;
    if (denom < 1e-6)
        return vec3(0.0, 0.0, -1.0);
    // Calibration image axes (u right, v down): our lens frame has both
    // flipped. See insv::LensProjection.
    float x = -d.x / denom, y = -d.y / denom;
    float r2 = x * x + y * y, r4 = r2 * r2;
    float radial = 1.0 + k.x * r2 + k.y * r4 + k.z * r2 * r4 + k.w * r4 * r4;
    float xd = x * radial + 2.0 * pd.x * x * y + pd.y * (r2 + 2.0 * x * x) + pd.z * r2 + pd.w * r4;
    float yd = y * radial + pd.x * (r2 + 2.0 * y * y) + 2.0 * pd.y * x * y + s34.x * r2 + s34.y * r4;
    float acc = accessoryScale(theta);
    return vec3(cx + fx * xd * acc, cy + fy * yd * acc, theta);
}

// Rotate v by -|r| about r, i.e. apply exp(r)^T.
vec3 rotateBack(vec3 v, vec3 r)
{
    float a = length(r);
    if (a < 1e-9)
        return v;
    vec3 ax = -r / a;
    return v * cos(a) + cross(ax, v) * sin(a) + ax * dot(ax, v) * (1.0 - cos(a));
}

// project() with the capture-time correction: lens i's row at fraction s of
// the sensor was captured when the camera had turned by R = exp(r(s)) from the
// frame time (r interpolated from lens i's rsRot taps, tap k at s =
// k/(RS_TAPS-1)), so the scene direction sat at R^T * world in the frame-time
// body frame. The row comes from a first projection with the lens's centre
// tap; one fixed-point step (the rotations are well under a degree).
vec3 projectRS(int i, vec3 world)
{
    if (P.rs.x == 0.0)
        return project(i, world);
    int base = i * RS_TAPS;
    vec3 pr = project(i, rotateBack(world, P.rsRot[base + RS_TAPS / 2].xyz));
    if (pr.z < 0.0)
        return pr;
    float s = (P.rs.y != 0.0) ? pr.x / float(P.dims.x) : pr.y / float(P.dims.y);
    float f = clamp(s, 0.0, 1.0) * float(RS_TAPS - 1);
    int k = min(int(f), RS_TAPS - 2);
    vec3 r = mix(P.rsRot[base + k].xyz, P.rsRot[base + k + 1].xyz, f - float(k));
    return project(i, rotateBack(world, r));
}

float seamWeight(float theta)
{
    float seamRad = P.seam.x;
    float bandRad = P.seam.y;
    if (theta <= seamRad - bandRad)
        return 1.0;
    if (theta >= seamRad + bandRad)
        return 0.0;
    float t = (theta - (seamRad - bandRad)) / (2.0 * bandRad);
    return 0.5 * (1.0 + cos(PI * t));
}

// Fade a lens out as its sample radius nears the imaged-circle edge (r/radius),
// so near-pole wide-angle rays don't pull in the black/vignette ring.
float circleEdgeWeight(float rr)
{
    float s = P.edge.x, e = P.edge.y;
    if (rr <= s)
        return 1.0;
    if (rr >= e || e <= s)
        return (rr >= e) ? 0.0 : 1.0;
    float t = (rr - s) / (e - s);
    return 0.5 * (1.0 + cos(PI * t));
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Equirect texel (x, y) of a W x H equirect -> stitch-world direction.
vec3 eqTexelDir(ivec2 t, int W, int H)
{
    float lat = (0.5 - (float(t.y) + 0.5) / float(H)) * PI;
    float lon = ((float(t.x) + 0.5) / float(W) - 0.5) * 2.0 * PI;
    float cl = cos(lat), sl = sin(lat);
    return mul3(P.outRot[0], P.outRot[1], P.outRot[2], vec3(cl * sin(lon), sl, -cl * cos(lon)));
}

// --- depth-aware seam (insv::FisheyeStitcher::seamQAt / lensRay) ----------

float seamQBin(int b) { return P.seamQ[b >> 2][b & 3]; }

// Where `dir` is on the seam (insv::FisheyeStitcher::seamPosition): x the
// elevation off the seam circle (rad, towards lens 0), y the bin position
// (bin centres at integers; i0 = floor, i1 = i0 + 1 wrapped).
vec2 seamPosition(vec3 dir, out int i0, out int i1)
{
    float e = asin(clamp(dot(dir, P.seamA.xyz), -1.0, 1.0));
    float phi = atan(dot(dir, P.seamV.xyz), dot(dir, P.seamU.xyz));
    if (phi < 0.0)
        phi += 2.0 * PI;
    float f = phi / (2.0 * PI) * float(SEAM_BINS) - 0.5;
    float fl = floor(f);
    i0 = (int(fl) + SEAM_BINS) % SEAM_BINS;
    i1 = (i0 + 1) % SEAM_BINS;
    return vec2(e, f - fl);
}

// The depth table at a seamPosition(): linear in azimuth, faded to 0 over
// the ramp past the blend band.
float seamQFrom(vec2 pos, int i0, int i1)
{
    float e = abs(pos.x);
    float band = P.seam.y, ramp = P.seamV.w;
    if (e >= band + ramp)
        return 0.0;
    float r = e > band ? 0.5 * (1.0 + cos(PI * (e - band) / ramp)) : 1.0;
    return r * mix(seamQBin(i0), seamQBin(i1), pos.y);
}

// Inverse stitch distance for stitch-world direction `dir`.
float seamQAt(vec3 dir)
{
    if (P.seamA.w == 0.0)
        return 0.0;
    int i0, i1;
    vec2 pos = seamPosition(dir, i0, i1);
    return seamQFrom(pos, i0, i1);
}

// The direction lens i sees the point at distance 1/q along `dir` in.
vec3 lensRay(int i, vec3 dir, float q)
{
    if (q == 0.0)
        return dir;
    float h = i == 0 ? P.seamU.w : -P.seamU.w;
    return normalize(dir - (q * h) * P.seamA.xyz);
}

// --- the new stitcher's seam flow (insv::FisheyeStitcher::seamFlowRay) ----

// The seam frame's direction at azimuth phi, elevation e (towards lens 0)
// (insv::FisheyeStitcher::seamDir).
vec3 seamDirAt(float phi, float e)
{
    return cos(e) * (cos(phi) * P.seamU.xyz + sin(phi) * P.seamV.xyz) + sin(e) * P.seamA.xyz;
}

// The direction lens i is sampled in for `dir`: moved along the seam frame by
// its share of the flow there -- lens 0 b(e) of F10, lens 1 1 - b(e) of F01
// (insv::SeamFlowShare: 0 at +reach, 1/2 on the seam, 1 at -reach); `dir`
// past the reach or with the flow off.
vec3 seamFlowRay(int i, vec3 dir)
{
    float reach = P.seamFlow.z;
    float sa = dot(dir, P.seamA.xyz);
    if (P.seamFlow.x == 0.0 || abs(sa) >= sin(reach))
        return dir;
    float e = asin(sa);
    float phi = atan(dot(dir, P.seamV.xyz), dot(dir, P.seamU.xyz));
    if (phi < 0.0)
        phi += 2.0 * PI;
    float b = 0.5 * (1.0 - sin(0.5 * PI * e / reach));
    float share = i == 0 ? b : 1.0 - b;
    // Texel centres at phi = (x + 0.5) 2 pi / cols (the sampler wraps round)
    // and e = strip - y 2 strip / (rows - 1), clamped at the strip's edges
    // (the texture's last row, under them, is the fade's).
    float strip = P.seamFlow.y;
    float rows = float(P.seamFlowN.y);
    float y = clamp((strip - e) * (rows - 1.0) / (2.0 * strip), 0.0, rows - 1.0);
    vec4 f = textureLod(seamFlowTex, vec2(phi / (2.0 * PI), (y + 0.5) / (rows + 1.0)), 0.0);
    vec2 d = i == 0 ? f.xy : f.zw;
    return seamDirAt(phi + share * d.x, e + share * d.y);
}

// Seam feather of lens i for `dir`, from dir's own off-axis angle whatever
// the depth (so the blend stays put when the rays shift).
float seamBlendWeight(int i, vec3 dir)
{
    vec3 z = i == 0 ? P.w2l0[2].xyz : P.w2l1[2].xyz; // the lens's optical axis
    return seamWeight(acos(clamp(dot(z, dir), -1.0, 1.0)));
}

vec2 seamCFBin(int b) { vec4 v = P.seamCF[b >> 1]; return (b & 1) == 0 ? v.xy : v.zw; }

// The new stitcher's veiling light for `dir` (lens 0 less lens 1;
// insv::FisheyeStitcher::seamVeil): the per-bin table interpolated along the
// seam, in the band and faded out over LENS_COLOUR_LOCAL_RAMP_RAD past it.
vec3 seamVeil(vec3 dir)
{
    int i0, i1;
    vec2 pos = seamPosition(dir, i0, i1);
    float e = abs(pos.x), band = P.seam.y;
    if (e >= band + LENS_COLOUR_LOCAL_RAMP_RAD)
        return vec3(0.0);
    float a = e > band ? 0.5 * (1.0 + cos(PI * (e - band) / LENS_COLOUR_LOCAL_RAMP_RAD)) : 1.0;
    return a * mix(P.lensVeil[i0].xyz, P.lensVeil[i1].xyz, pos.y);
}

// The new stitcher's fade share at `dir`'s azimuth (insv::FisheyeStitcher::
// seamFadeShare): the seam flow texture's last row, linear between columns.
float seamFadeShare(vec3 dir)
{
    float phi = atan(dot(dir, P.seamV.xyz), dot(dir, P.seamU.xyz));
    if (phi < 0.0)
        phi += 2.0 * PI;
    float rows = float(P.seamFlowN.y);
    return textureLod(seamFlowTex, vec2(phi / (2.0 * PI), (rows + 0.5) / (rows + 1.0)), 0.0).x;
}

// The two lenses' feather at a seamPosition() (insv::FisheyeStitcher::
// seamBlendWeights): the cosine ramp round the bin's cut with its own
// half-width (seamCF), else -- not measured yet -- the plain cross-fade.
vec2 seamFadeWeights(vec3 dir, vec2 pos, int i0, int i1, bool flowFade)
{
    vec2 c0 = seamCFBin(i0), c1 = seamCFBin(i1);
    if (c0.y > 0.0 && c1.y > 0.0) {
        vec2 cf = mix(c0, c1, pos.y);
        if (flowFade) {
            float narrow = min(SEAM_CUT_FADE_RAD, P.seam.y); // insv::SeamFadeRad
            cf.y = max(cf.y, narrow + (P.seam.y - narrow) * seamFadeShare(dir));
        }
        float x = (pos.x - cf.x) / cf.y;
        float w0 = x >= 1.0 ? 1.0 : (x <= -1.0 ? 0.0 : 0.5 * (1.0 + sin(0.5 * PI * x)));
        return vec2(w0, 1.0 - w0);
    }
    return vec2(seamBlendWeight(0, dir), seamBlendWeight(1, dir));
}

// The seam's per-bin tables at `dir`, its position found once: returns the
// inverse stitch distance (insv::FisheyeStitcher::seamQAt); `w` the two
// lenses' feather -- with the depth-aware seam its cut, with the new
// stitcher's seam colour blend the narrow fade its veiling light widens
// (insv::SeamVeilFade; both in seamCF, seamFadeWeights), else the plain
// cross-fade; `local` the lens colour's local correction there and `veil`
// the veiling light (lens 0 less lens 1), in the band and faded out past it
// (insv::FisheyeStitcher::lensLogGains), 0 without the seam -- or the new
// stitcher's with its seam colour blend (seamVeil).
float seamTables(vec3 dir, out vec2 w, out vec3 local, out vec3 veil)
{
    local = vec3(0.0);
    veil = vec3(0.0);
    if (P.seamA.w == 0.0) {
        if (P.edge.z != 0.0) {
            int i0, i1;
            vec2 pos = seamPosition(dir, i0, i1);
            // The narrow fade with the seam flow's (seamCF from the veiling
            // light, insv::SeamVeilFade; seamFadeShare), else the plain one.
            w = P.seamFlow.x != 0.0 ? seamFadeWeights(dir, pos, i0, i1, true)
                                    : vec2(seamBlendWeight(0, dir), seamBlendWeight(1, dir));
            veil = seamVeil(dir);
        } else {
            w = vec2(seamBlendWeight(0, dir), seamBlendWeight(1, dir));
        }
        return 0.0;
    }
    int i0, i1;
    vec2 pos = seamPosition(dir, i0, i1);
    w = seamFadeWeights(dir, pos, i0, i1, false);
    float e = abs(pos.x), band = P.seam.y;
    if (e < band + LENS_COLOUR_LOCAL_RAMP_RAD) {
        float a = e > band ? 0.5 * (1.0 + cos(PI * (e - band) / LENS_COLOUR_LOCAL_RAMP_RAD)) : 1.0;
        local = a * mix(P.lensLocal[i0].xyz, P.lensLocal[i1].xyz, pos.y);
        veil = a * mix(P.lensVeil[i0].xyz, P.lensVeil[i1].xyz, pos.y);
    }
    return seamQFrom(pos, i0, i1);
}

// The lens colour's log ratio L (lens 0 / lens 1) for `dir`
// (insv::FisheyeStitcher::lensLogGains): at dir's elevation off the seam
// circle, held at the band's edge beyond it, plus the `local` correction
// (seamTables); lens 0 takes -L / 2, lens 1 L / 2.
vec3 lensColourLog(vec3 dir, vec3 local)
{
    float e = clamp(asin(clamp(dot(dir, P.seamA.xyz), -1.0, 1.0)), -P.seam.y, P.seam.y);
    vec3 l = clamp(P.lensCol.xyz + P.lensColS.xyz * e, vec3(-LENS_COLOUR_MAX_LOG),
                   vec3(LENS_COLOUR_MAX_LOG));
    return clamp(l + local, vec3(-LENS_COLOUR_MAX_LOG), vec3(LENS_COLOUR_MAX_LOG));
}

// The seam colour blend's log ratio (lens 0 / lens 1) for `dir`
// (insv::FisheyeStitcher::seamColourLog): the per-bin table interpolated
// along the seam, whole to SEAM_COLOUR_FULL_RAD off it, a cosine to none at
// SEAM_COLOUR_END_RAD (insv::SeamColourFade). 0 when off.
vec3 seamColourLog(vec3 dir)
{
    if (P.edge.z == 0.0)
        return vec3(0.0);
    int i0, i1;
    vec2 pos = seamPosition(dir, i0, i1);
    float e = abs(pos.x);
    if (e >= SEAM_COLOUR_END_RAD)
        return vec3(0.0);
    float a = e > SEAM_COLOUR_FULL_RAD
                  ? 0.5 * (1.0 + cos(PI * (e - SEAM_COLOUR_FULL_RAD) /
                                     (SEAM_COLOUR_END_RAD - SEAM_COLOUR_FULL_RAD)))
                  : 1.0;
    return a * mix(P.seamCol[i0].xyz, P.seamCol[i1].xyz, pos.y);
}

// A lens sample `s` (0..255) with the veiling light `veil` (linear, >= 0)
// taken out (insv::LensVeilRemove), then log gain `lg` per channel, both
// eased off over the highlight knee (insv::LensColourGain).
vec3 lensColourApply(vec3 s, vec3 lg, vec3 veil)
{
    vec3 knee = 1.0 - smoothstep(vec3(LENS_COLOUR_KNEE_LO), vec3(LENS_COLOUR_KNEE_HI), s);
    if (any(greaterThan(veil, vec3(0.0)))) {
        vec3 lin = pow(clamp(s / 255.0, 0.0, 1.0), vec3(LENS_VEIL_GAMMA));
        vec3 v = 255.0 * pow(max(lin - veil * knee, lin * LENS_VEIL_KEEP), vec3(1.0 / LENS_VEIL_GAMMA));
        s = mix(s, v, greaterThan(veil, vec3(0.0)));
        knee = 1.0 - smoothstep(vec3(LENS_COLOUR_KNEE_LO), vec3(LENS_COLOUR_KNEE_HI), s);
    }
    return s * exp(lg * knee);
}

// Per-lens sample uv + blend weight for stitch-world direction `dir` (a
// weight of 0 means that lens doesn't contribute), with the seam's per-bin
// blend (seamTables), and the lens colour's local correction and the
// veiling light there.
void lensWeights(vec3 dir, out vec2 uv0, out vec2 uv1, out float w0, out float w1,
                 out vec3 local, out vec3 veil)
{
    float fw = float(P.dims.x), fh = float(P.dims.y);
    uv0 = vec2(0.0);
    uv1 = vec2(0.0);
    w0 = 0.0;
    w1 = 0.0;
    vec2 wsl;
    float q = seamTables(dir, wsl, local, veil);
    for (int i = 0; i < 2; ++i) {
        float ws = wsl[i];
        if (ws <= 0.0)
            continue;
        vec3 pr = projectRS(i, P.seamFlow.x != 0.0 ? seamFlowRay(i, dir) : lensRay(i, dir, q));
        if (pr.z < 0.0)
            continue;
        float u = pr.x, v = pr.y;
        if (u < 0.0 || v < 0.0 || u > fw - 1.0 || v > fh - 1.0)
            continue;
        float wt = ws * circleEdgeWeight(radiusForTheta(pr.z));
        if (wt <= 0.0)
            continue;
        vec2 uv = (vec2(u, v) + 0.5) / vec2(fw, fh);
        if (i == 0) { uv0 = uv; w0 = wt; }
        else        { uv1 = uv; w1 = wt; }
    }
}

#ifndef FISHEYE_STITCH_NO_BUFFERS
// Pass 2: the depth-aware seam's cost volume. Workgroup (bin, k) of 64
// invocations samples the bin's SEAM_PATCH x SEAM_PATCH patch (4 samples
// each) from both lenses, each from its own centre at candidate distance k,
// and writes the grey NCC + texture with each lens's brightness plane taken
// out -- insv::MeasureSeamCosts on the GPU (same sample pattern,
// insv::SeamPatchSample; same sums and score, insv::SeamPatchAccumulate /
// insv::SeamPatchScore).
#define SEAM_SUMS 15 // == insv::kSeamPatchSums
shared float seamRed[SEAM_SUMS][64]; // pass 2 uses all rows, pass 3 the first 9

void seamCostPass(uvec3 wg, uint lid)
{
    int bin = int(wg.x), k = int(wg.y);
    if (bin >= SEAM_BINS || k >= SEAM_CANDS)
        return;
    float q = mix(P.seamC.x, P.seamC.y, float(k) / float(SEAM_CANDS - 1));
    float fw = float(P.dims.x), fh = float(P.dims.y);
    float acc[SEAM_SUMS];
    for (int c = 0; c < SEAM_SUMS; ++c)
        acc[c] = 0.0;
    for (int j = 0; j < SEAM_PATCH * SEAM_PATCH / 64; ++j) {
        int s = int(lid) + 64 * j;
        int az = s % SEAM_PATCH, el = s / SEAM_PATCH;
        float phi = (float(bin) + (float(az) + 0.5) / float(SEAM_PATCH)) * (2.0 * PI / float(SEAM_BINS));
        float e = (-1.0 + (2.0 * float(el) + 1.0) / float(SEAM_PATCH)) * P.seamC.z;
        vec3 dir = cos(e) * (cos(phi) * P.seamU.xyz + sin(phi) * P.seamV.xyz) + sin(e) * P.seamA.xyz;
        vec3 p0 = projectRS(0, lensRay(0, dir, q));
        vec3 p1 = projectRS(1, lensRay(1, dir, q));
        if (p0.z < 0.0 || p1.z < 0.0)
            continue;
        if (p0.x < 0.0 || p0.y < 0.0 || p0.x > fw - 1.0 || p0.y > fh - 1.0 ||
            p1.x < 0.0 || p1.y < 0.0 || p1.x > fw - 1.0 || p1.y > fh - 1.0)
            continue;
        // Grey less mid-grey: the plane absorbs the offset, and float sums of
        // squares lose less to it (the CPU sums in double).
        float x = luma(sampleLens(0, (p0.xy + 0.5) / vec2(fw, fh))) - 128.0;
        float y = luma(sampleLens(1, (p1.xy + 0.5) / vec2(fw, fh))) - 128.0;
        float a = float(az) - 0.5 * float(SEAM_PATCH - 1);
        float b = float(el) - 0.5 * float(SEAM_PATCH - 1);
        acc[0] += 1.0; acc[1] += a; acc[2] += b; acc[3] += a * a; acc[4] += b * b; acc[5] += a * b;
        acc[6] += x; acc[7] += x * a; acc[8] += x * b; acc[9] += x * x;
        acc[10] += y; acc[11] += y * a; acc[12] += y * b; acc[13] += y * y;
        acc[14] += x * y;
    }
    for (int c = 0; c < SEAM_SUMS; ++c)
        seamRed[c][lid] = acc[c];
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1) {
        if (lid < stride)
            for (int c = 0; c < SEAM_SUMS; ++c)
                seamRed[c][lid] += seamRed[c][lid + stride];
        barrier();
    }
    if (lid != 0u)
        return;
    float n = seamRed[0][0];
    vec2 res = vec2(-2.0, 0.0);
    if (n >= float(SEAM_PATCH * SEAM_PATCH / 4)) {
        // M = R^T R for the regressors (1, az, el), inverted by cofactors.
        float m01 = seamRed[1][0], m02 = seamRed[2][0];
        float m11 = seamRed[3][0], m22 = seamRed[4][0], m12 = seamRed[5][0];
        float c00 = m11 * m22 - m12 * m12, c01 = m02 * m12 - m01 * m22;
        float c02 = m01 * m12 - m02 * m11, c11 = n * m22 - m02 * m02;
        float c12 = m01 * m02 - n * m12, c22 = n * m11 - m01 * m01;
        float det = n * c00 + m01 * c01 + m02 * c02;
        if (abs(det) >= 1e-9 * n * n * n) {
            mat3 inv = mat3(c00, c01, c02, c01, c11, c12, c02, c12, c22) / det;
            vec3 bx = vec3(seamRed[6][0], seamRed[7][0], seamRed[8][0]);
            vec3 by = vec3(seamRed[10][0], seamRed[11][0], seamRed[12][0]);
            float vx = max(seamRed[9][0] - dot(bx, inv * bx), 0.0) / n;
            float vy = max(seamRed[13][0] - dot(by, inv * by), 0.0) / n;
            float cxy = (seamRed[14][0] - dot(bx, inv * by)) / n;
            res.y = sqrt(min(vx, vy));
            // == insv::kSeamMinVariance: a flat patch's variance is float noise.
            res.x = (vx > 0.25 && vy > 0.25) ? clamp(cxy / sqrt(vx * vy), -1.0, 1.0) : 0.0;
        } else {
            res.x = -2.0;
        }
    }
    seamCost[bin * SEAM_CANDS + k] = res;
}

// Pass 3: the seam cut's statistics. Workgroup (bin, row) of 64 invocations,
// one sample of the row's patch each, both lenses warped by the depth table
// in P.seamQ (the frame before): the zero-mean grey difference and the
// texture -- insv::MeasureSeamCutStats on the GPU (same sample pattern,
// insv::SeamCutSample). Reduces in pass 2's shared memory.
void seamCutPass(uvec3 wg, uint lid)
{
    int bin = int(wg.x), row = int(wg.y);
    if (bin >= SEAM_BINS || row >= SEAM_ROWS)
        return;
    float fw = float(P.dims.x), fh = float(P.dims.y);
    int az = int(lid) % SEAM_CUT_AZ, el = int(lid) / SEAM_CUT_AZ;
    float rowMax = P.seamC.w;
    float phi = (float(bin) + (float(az) + 0.5) / float(SEAM_CUT_AZ)) * (2.0 * PI / float(SEAM_BINS));
    float e = -rowMax + 2.0 * rowMax * float(row) / float(SEAM_ROWS - 1) +
              (-1.0 + (2.0 * float(el) + 1.0) / float(SEAM_CUT_EL)) * SEAM_CUT_EL_RAD;
    vec3 dir = cos(e) * (cos(phi) * P.seamU.xyz + sin(phi) * P.seamV.xyz) + sin(e) * P.seamA.xyz;
    float q = seamQAt(dir);
    vec3 p0 = projectRS(0, lensRay(0, dir, q));
    vec3 p1 = projectRS(1, lensRay(1, dir, q));
    bool ok = p0.z >= 0.0 && p1.z >= 0.0 &&
              p0.x >= 0.0 && p0.y >= 0.0 && p0.x <= fw - 1.0 && p0.y <= fh - 1.0 &&
              p1.x >= 0.0 && p1.y >= 0.0 && p1.x <= fw - 1.0 && p1.y <= fh - 1.0;
    float x = 0.0, y = 0.0;
    vec3 c0 = vec3(0.0), c1 = vec3(0.0);
    if (ok) {
        c0 = sampleLens(0, (p0.xy + 0.5) / vec2(fw, fh));
        c1 = sampleLens(1, (p1.xy + 0.5) / vec2(fw, fh));
        x = luma(c0);
        y = luma(c1);
    }
    float n1 = ok ? 1.0 : 0.0;
    // Colour per channel only where neither lens has it clipped.
    vec3 nc1 = ok ? vec3(lessThanEqual(max(c0, c1), vec3(SEAM_CUT_CLIP))) : vec3(0.0);
    seamRed[0][lid] = x;
    seamRed[1][lid] = y;
    seamRed[2][lid] = n1;
    for (int c = 0; c < 3; ++c) {
        seamRed[3 + c][lid] = nc1[c];
        seamRed[6 + c][lid] = nc1[c] * c0[c];
    }
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1) {
        if (lid < stride)
            for (int c = 0; c < 9; ++c)
                seamRed[c][lid] += seamRed[c][lid + stride];
        barrier();
    }
    float n = seamRed[2][0];
    vec3 nc = vec3(seamRed[3][0], seamRed[4][0], seamRed[5][0]);
    float mx = n > 0.0 ? seamRed[0][0] / n : 0.0, my = n > 0.0 ? seamRed[1][0] / n : 0.0;
    vec3 m0 = vec3(seamRed[6][0], seamRed[7][0], seamRed[8][0]) / max(nc, vec3(1.0));
    barrier(); // everyone has read the sums before they're overwritten
    float dx = x - mx, dy = y - my;
    seamRed[0][lid] = n1 * abs(dx - dy);
    seamRed[1][lid] = n1 * dx * dx;
    seamRed[2][lid] = n1 * dy * dy;
    for (int c = 0; c < 3; ++c)
        seamRed[3 + c][lid] = nc1[c] * c1[c];
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1) {
        if (lid < stride)
            for (int c = 0; c < 6; ++c)
                seamRed[c][lid] += seamRed[c][lid + stride];
        barrier();
    }
    if (lid != 0u)
        return;
    vec4 res = vec4(n, 0.0, 0.0, 0.0);
    if (n > 0.0) {
        res.y = seamRed[0][0] / n;
        res.z = sqrt(min(seamRed[1][0], seamRed[2][0]) / n);
    }
    vec3 m1 = vec3(seamRed[3][0], seamRed[4][0], seamRed[5][0]) / max(nc, vec3(1.0));
    int o = 4 * (bin * SEAM_ROWS + row);
    seamCutStat[o] = res;
    seamCutStat[o + 1] = vec4(nc, 0.0);
    seamCutStat[o + 2] = vec4(m0, 0.0);
    seamCutStat[o + 3] = vec4(m1, 0.0);
}
#endif

#ifndef FISHEYE_STITCH_NO_BUFFERS
// Pass 4: the seam flow's strips (insv::MeasureSeamFlowStrips, here with the
// capture-time correction, as the stitch samples). Invocation (gx, y) does
// columns 4 gx .. 4 gx + 3 of row y of both lenses; grey is BT.601 luma.
void seamStripPass(ivec2 gid)
{
    int cols = P.seamFlowN.x, rows = P.seamFlowN.y;
    if (gid.x * 4 >= cols || gid.y >= rows)
        return;
    float strip = P.seamFlow.y;
    float e = strip - float(gid.y) * 2.0 * strip / float(rows - 1);
    float fw = float(P.dims.x), fh = float(P.dims.y);
    for (int i = 0; i < 2; ++i) {
        uint packed = 0u;
        for (int k = 0; k < 4; ++k) {
            float phi = (float(gid.x * 4 + k) + 0.5) * 2.0 * PI / float(cols);
            vec3 pr = projectRS(i, seamDirAt(phi, e));
            float g = 0.0;
            if (pr.z >= 0.0 && pr.x >= 0.0 && pr.y >= 0.0 && pr.x <= fw - 1.0 && pr.y <= fh - 1.0)
                g = clamp(luma(sampleLens(i, (pr.xy + 0.5) / vec2(fw, fh))) + 0.5, 0.0, 255.0);
            packed |= uint(g) << (8 * k);
        }
        seamStrip[i * (rows * cols / 4) + (gid.y * cols + gid.x * 4) / 4] = packed;
    }
}
#endif

// The stitched colour for `dir`, RGB in [0, 255] (unquantised); black outside
// both imaged circles. Each lens's colour matched by the lens colour
// (lensColourLog) and the seam colour blend (seamColourLog), both lenses to
// the geometric mean of their colours.
vec3 blendLenses(vec3 dir)
{
    vec2 uv0, uv1;
    float w0, w1;
    vec3 local, veil;
    lensWeights(dir, uv0, uv1, w0, w1, local, veil);
    vec3 lg = 0.5 * lensColourLog(dir, local);
    if (P.edge.z != 0.0)
        lg += 0.5 * seamColourLog(dir);
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    if (w0 > 0.0) { acc += w0 * lensColourApply(sampleLens(0, uv0), -lg, max(veil, 0.0)); wsum += w0; }
    if (w1 > 0.0) { acc += w1 * lensColourApply(sampleLens(1, uv1), lg, max(-veil, 0.0)); wsum += w1; }
    return wsum > 1e-6 ? acc / wsum : vec3(0.0);
}

// Privacy blur (insv/BlurQuads.h, mirrored there and in
// FisheyeStitcher::renderEquirect): inside a blur quad the colour is the
// quad's mosaic cell -- the mean of BLUR_TAPS^2 directions spread over the
// cell -- faded into the picture over the outer BLUR_FEATHER of (s, t).
float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

// insv::blurQuadST: where `dir` falls in quad k, (s, t) in [0, 1]^2 inside.
bool blurST(int k, vec3 dir, out vec2 st)
{
    st = vec2(-1.0);
    vec4 A = P.blurQ[k * 6], B = P.blurQ[k * 6 + 1], C = P.blurQ[k * 6 + 2];
    float z = dot(dir, A.xyz);
    if (z < C.w || z <= 0.0)
        return false;
    vec4 D = P.blurQ[k * 6 + 3], E = P.blurQ[k * 6 + 4];
    float scale = P.blurQ[k * 6 + 5].x;
    vec2 p = vec2(dot(dir, B.xyz), dot(dir, C.xyz)) / (z * scale);
    vec2 a = D.xy;
    vec2 e = D.zw - a, f = E.zw - a, g = a - D.zw + E.xy - E.zw, h = p - a;
    float k2 = cross2(g, f);
    float k1 = cross2(e, f) + cross2(h, g);
    float k0 = cross2(h, e);
    float t0, t1;
    if (abs(k2) < 1e-7) {
        if (abs(k1) < 1e-12)
            return false;
        t0 = t1 = -k0 / k1;
    } else {
        float w = k1 * k1 - 4.0 * k0 * k2;
        if (w < 0.0)
            return false;
        w = sqrt(w);
        t0 = (-k1 - w) * 0.5 / k2;
        t1 = (-k1 + w) * 0.5 / k2;
    }
    for (int i = 0; i < 2; ++i) {
        float t = i == 0 ? t0 : t1;
        vec2 dd = e + g * t;
        float s = abs(dd.x) > abs(dd.y) ? (h.x - f.x * t) / dd.x : (h.y - f.y * t) / dd.y;
        if (s >= 0.0 && s <= 1.0 && t >= 0.0 && t <= 1.0) {
            st = vec2(s, t);
            return true;
        }
    }
    return false;
}

// insv::blurQuadDir: (s, t) in quad k -> its direction.
vec3 blurDir(int k, vec2 st)
{
    vec4 A = P.blurQ[k * 6], B = P.blurQ[k * 6 + 1], C = P.blurQ[k * 6 + 2];
    vec4 D = P.blurQ[k * 6 + 3], E = P.blurQ[k * 6 + 4];
    float scale = P.blurQ[k * 6 + 5].x;
    vec2 top = mix(D.xy, D.zw, st.x), bot = mix(E.zw, E.xy, st.x);
    vec2 p = mix(top, bot, st.y) * scale;
    return normalize(A.xyz + B.xyz * p.x + C.xyz * p.y);
}

// blendLenses() with the privacy blur applied: what every render path calls.
vec3 blurredLenses(vec3 dir)
{
    vec3 rgb = blendLenses(dir);
    int n = int(P.blur0.x);
    for (int k = 0; k < n; ++k) {
        vec2 st;
        if (!blurST(k, dir, st))
            continue;
        vec2 cells = vec2(P.blurQ[k * 6].w, P.blurQ[k * 6 + 1].w);
        vec2 cell = min(floor(st * cells), cells - 1.0);
        vec3 mos = vec3(0.0);
        for (int i = 0; i < BLUR_TAPS; ++i)
            for (int j = 0; j < BLUR_TAPS; ++j)
                mos += blendLenses(blurDir(k, (cell + (vec2(i, j) + 0.5) / float(BLUR_TAPS)) / cells));
        mos /= float(BLUR_TAPS * BLUR_TAPS);
        float edge = min(min(st.x, 1.0 - st.x), min(st.y, 1.0 - st.y));
        return mix(rgb, mos, smoothstep(0.0, BLUR_FEATHER, edge));
    }
    return rgb;
}

#ifndef FISHEYE_STITCH_NO_MAIN
void main()
{
    if (pc.pass == 2) {
        seamCostPass(gl_WorkGroupID, gl_LocalInvocationIndex);
        return;
    }
    if (pc.pass == 3) {
        seamCutPass(gl_WorkGroupID, gl_LocalInvocationIndex);
        return;
    }
    if (pc.pass == 4) {
        seamStripPass(ivec2(gl_GlobalInvocationID.xy));
        return;
    }
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    int W = P.dims.z, H = P.dims.w;
    if (gid.x >= W || gid.y >= H)
        return;
    vec3 dir = eqTexelDir(gid, W, H);
    uvec3 q = uvec3(clamp(blurredLenses(dir) + 0.5, vec3(0.0), vec3(255.0)));
    px[gid.y * W + gid.x] = q.r | (q.g << 8) | (q.b << 16) | (255u << 24);
}
#endif
