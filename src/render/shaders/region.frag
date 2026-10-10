#version 450

// The outline of one privacy-blur region over the view (the Privacy panel's
// "Show regions"): a polygon in output-rect pixels -- the region's quad,
// projected and clipped to what lies in front of the camera, so 3 to 6
// corners. The fullscreen triangle is scissored to the polygon's bounds; this
// shades the pixels within half the outline width of an edge and discards
// the rest. No texture.

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec2 origin;  // the output rect's top-left in framebuffer pixels
    float border; // outline width, px
    float count;  // corners in use
    vec4 color;   // rgb; a = opacity
    vec4 pts[3];  // corners in output-rect px, two per vec4 (xy, zw)
} pc;

vec2 corner(int i) {
    vec4 v = pc.pts[i >> 1];
    return (i & 1) == 0 ? v.xy : v.zw;
}

void main() {
    vec2 p = gl_FragCoord.xy - pc.origin; // pixel centre, output-rect px
    int n = int(pc.count);
    float d = 1e9;
    for (int i = 0; i < n; ++i) {
        vec2 a = corner(i), b = corner((i + 1) % n);
        vec2 ab = b - a;
        float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        d = min(d, length(p - (a + t * ab)));
    }
    float alpha = clamp(pc.border * 0.5 + 0.5 - d, 0.0, 1.0); // 1 px antialiased edge
    if (alpha <= 0.0)
        discard;
    outColor = vec4(pc.color.rgb, pc.color.a * alpha);
}
