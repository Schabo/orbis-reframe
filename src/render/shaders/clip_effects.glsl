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
// outer radius, unused}, the radius 1 in the frame's corners. `grain` is
// fx::Render::grain: {spread (fraction of white, 0 = off), grains per frame
// height, colour 0..1, frame number}.
#ifdef EFFECTS
layout(set = 2, binding = 0) uniform ClipEffects {
    vec4 frame;
    vec4 vignette;
    vec4 grain;
} fx;

// fx::grainHash / grainCell / grainNoise / grainSeed.
uint grainHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float grainCell(int cx, int cy, uint seed)
{
    uint h = grainHash(uint(cx) * 0x9E3779B1u ^ grainHash(uint(cy) ^ seed));
    return (float(h & 0xFFFFu) + float(h >> 16)) / 65535.0 - 1.0;
}

float grainNoise(vec2 q, uint seed)
{
    vec2 f = q - 0.5;
    vec2 i = floor(f);
    vec2 t = f - i;
    t = t * t * (3.0 - 2.0 * t);
    ivec2 c = ivec2(i);
    float a = grainCell(c.x, c.y, seed), b = grainCell(c.x + 1, c.y, seed);
    float d0 = grainCell(c.x, c.y + 1, seed), d1 = grainCell(c.x + 1, c.y + 1, seed);
    float top = a + (b - a) * t.x, bottom = d0 + (d1 - d0) * t.x;
    float v = top + (bottom - top) * t.y;
    vec2 n = sqrt((1.0 - t) * (1.0 - t) + t * t);
    return v / (n.x * n.y) * 2.4494897;
}

uint grainSeed(float frame, uint channel)
{
    return grainHash(uint(frame) * 4u + channel + 1u);
}

// `c` in [0, white]; `p` the pixel's centre in the shader's coordinates.
vec3 applyEffects(vec3 c, vec2 p, float white)
{
    if (fx.vignette.x != 0.0) {
        vec2 uv = 2.0 * (p - fx.frame.xy) / fx.frame.zw - 1.0;
        float r = sqrt(dot(uv, uv) * 0.5);
        float a = fx.vignette.x * smoothstep(fx.vignette.y, fx.vignette.z, r);
        c = a < 0.0 ? c * (1.0 + a) : c + (vec3(white) - c) * a;
    }
    if (fx.grain.x != 0.0) {
        vec2 q = (p - fx.frame.xy) * (fx.grain.y / fx.frame.w);
        float grey = grainNoise(q, grainSeed(fx.grain.w, 0u));
        vec3 n = vec3(grey);
        if (fx.grain.z > 0.0) {
            vec3 own = vec3(grainNoise(q, grainSeed(fx.grain.w, 1u)),
                            grainNoise(q, grainSeed(fx.grain.w, 2u)),
                            grainNoise(q, grainSeed(fx.grain.w, 3u)));
            n = sqrt(1.0 - fx.grain.z) * grey + sqrt(fx.grain.z) * own;
        }
        float l = clamp(dot(c, vec3(0.2126, 0.7152, 0.0722)) / white, 0.0, 1.0);
        float s = fx.grain.x * white * 4.0 * l * (1.0 - l);
        c = clamp(c + s * n, vec3(0.0), vec3(white));
    }
    return c;
}
#endif
