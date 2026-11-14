// A clip's video effects (src/core/ClipEffects.h), run right after the colour
// grade by the shaders that grade -- only in their EFFECTS build, so a
// picture without effects is drawn by code that doesn't contain them.
// fx::applyEffects() in ClipEffects.h is the C++ twin (the CPU export):
// change both together.
//
// The block (set 2, render::VulkanEffectsRing, bound with a dynamic offset
// per draw): `frame` is the whole output frame in the shader's own pixel
// coordinates -- x, y of its top-left corner, its width and height -- so an
// effect is placed against the frame, not the pane or inset being drawn.
// `vignette` is fx::Render::vignette: {amount -1..1 (0 = off), inner radius,
// outer radius, unused}, the radius 1 in the frame's corners.
#ifdef EFFECTS
layout(set = 2, binding = 0) uniform ClipEffects {
    vec4 frame;
    vec4 vignette;
} fx;

// `c` in [0, white]; `p` the pixel's centre in the shader's coordinates.
vec3 applyEffects(vec3 c, vec2 p, float white)
{
    if (fx.vignette.x != 0.0) {
        vec2 uv = 2.0 * (p - fx.frame.xy) / fx.frame.zw - 1.0;
        float r = sqrt(dot(uv, uv) * 0.5);
        float a = fx.vignette.x * smoothstep(fx.vignette.y, fx.vignette.z, r);
        c = a < 0.0 ? c * (1.0 + a) : c + (vec3(white) - c) * a;
    }
    return c;
}
#endif
