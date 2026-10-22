#version 450
#extension GL_GOOGLE_include_directive : require

// The viewport drawn straight from the fisheyes -- the fragment twin of
// fisheye_view_nv12.comp. Each pixel's view ray (pose, then stab; same math as
// equirect.frag) goes to the stitch direction and samples the two lenses there,
// so the picture has the source's full detail at any zoom and costs about
// window pixels.
//
// The lenses arrive as plain plane images shared from the decoder's device
// (render::LensFrame): Y (R8) and interleaved CbCr (R8G8, half size) per
// lens -- R16 / R16G16 for 10-bit pictures -- converted here with the
// stream's matrix and range (P.ycc) and the midpoint chroma siting the
// stitch's ycbcr sampler uses. Params (binding 0) is the
// stitch's own UBO for this frame, packed on the decoder side, and the
// photometric gains were measured there too.

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D lens0Y;
layout(binding = 2) uniform sampler2D lens0UV;
layout(binding = 3) uniform sampler2D lens1Y;
layout(binding = 4) uniform sampler2D lens1UV;

layout(push_constant) uniform PushConstants {
    float yaw;
    float pitch;
    float roll;
    float fovY;
    float aspect;
    float distortion; // view_projection.glsl's d
    // 8 bytes of std140 padding (mat3 aligns at offset 32).
    mat3 stab;       // gyro de-rotation in the sphere frame, after the pose
    vec4 colorGrade; // exposureEv, contrast%, saturation%, temperature%
    vec4 colorLut;   // the clip's LUT: mix, domain min / max (color_grade.glsl)
    vec4 gains;      // x, y: photometric gain of lens 0 / 1; z, w: -
} pc;

// Defined below the include: its conversion is in the include's Params (P.ycc).
vec3 sampleLens(int i, vec2 uv);

#define FISHEYE_STITCH_NO_MAIN
#define FISHEYE_STITCH_NO_BUFFERS
#include "fisheye_stitch_common.glsl"

// The stream's matrix and range, folded with the planes' bit depth into P.ycc
// on the decoder side (insv::YcbcrToRgbRows).
vec3 ycbcrToRgb(float y, vec2 cbcr)
{
    vec3 c = vec3(y, cbcr);
    return vec3(dot(P.ycc[0].xyz, c), dot(P.ycc[1].xyz, c), dot(P.ycc[2].xyz, c)) +
           vec3(P.ycc[0].w, P.ycc[1].w, P.ycc[2].w);
}

vec3 sampleLens(int i, vec2 uv)
{
    // The half-size CbCr texel centres sit at the middle of each 2x2 luma
    // block (midpoint siting), so plain bilinear sampling at `uv` matches the
    // ycbcr sampler's linear chroma reconstruction.
    vec3 c = i == 0 ? ycbcrToRgb(texture(lens0Y, uv).r, texture(lens0UV, uv).rg)
                    : ycbcrToRgb(texture(lens1Y, uv).r, texture(lens1UV, uv).rg);
    return clamp(c, 0.0, 1.0) * 255.0;
}

#include "color_grade.glsl"
#include "view_projection.glsl"

void main() {
    vec3 r = viewRayAt(inNdc, pc.fovY, pc.aspect, pc.distortion);

    float cr = cos(pc.roll), sr = sin(pc.roll);
    float cp = cos(pc.pitch), sp = sin(pc.pitch);
    float cy = cos(pc.yaw), sy = sin(pc.yaw);
    vec3 rr = vec3(r.x * cr - r.y * sr, r.x * sr + r.y * cr, r.z);
    vec3 rp = vec3(rr.x, rr.y * cp - rr.z * sp, rr.y * sp + rr.z * cp);
    vec3 sphereDir = normalize(pc.stab * vec3(rp.x * cy + rp.z * sy, rp.y, -rp.x * sy + rp.z * cy));

    vec3 dir = mul3(P.outRot[0], P.outRot[1], P.outRot[2], sphereDir);
    vec3 rgb = blurredLenses(dir, pc.gains.x, pc.gains.y) / 255.0;
    outColor = vec4(gradeColor(rgb, pc.colorGrade, pc.colorLut, 1.0), 1.0);
}
