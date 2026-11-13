#version 450

// The outline of one privacy-blur region over the view (the Privacy panel's
// "Show regions"): a polygon in output-rect pixels -- the region's quad,
// its edges followed along their arcs, clipped to what the view sees and
// projected (view::outlineToPlane). The frame's polygons share one buffer;
// this one is `count` points from `first`. The fullscreen triangle is
// scissored to the polygon's bounds; this shades the pixels within half the
// outline width of an edge and discards the rest. No texture.

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

layout(std430, set = 0, binding = 0) readonly buffer Points {
    vec2 pts[]; // output-rect px
};

layout(push_constant) uniform PushConstants {
    vec2 origin;  // the output rect's top-left in framebuffer pixels
    float border; // outline width, px
    int count;    // points in the polygon
    vec4 color;   // rgb; a = opacity
    int first;    // its first point in pts
} pc;

void main() {
    vec2 p = gl_FragCoord.xy - pc.origin; // pixel centre, output-rect px
    float d = 1e9;
    vec2 a = pts[pc.first + pc.count - 1];
    for (int i = 0; i < pc.count; ++i) {
        vec2 b = pts[pc.first + i];
        vec2 ab = b - a;
        float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        d = min(d, length(p - (a + t * ab)));
        a = b;
    }
    float alpha = clamp(pc.border * 0.5 + 0.5 - d, 0.0, 1.0); // 1 px antialiased edge
    if (alpha <= 0.0)
        discard;
    outColor = vec4(pc.color.rgb, pc.color.a * alpha);
}
