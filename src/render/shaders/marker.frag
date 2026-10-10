#version 450

// A rectangle outline over the view -- the box being dragged around a subject
// for "Track subject" (MainWindow::startSubjectTracking). The fullscreen
// triangle covers the output rect; this shades the outline (plus a faint fill)
// and discards the rest. No texture.

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec4 rect;   // x, y, w, h in output-rect pixels
    vec2 origin; // the output rect's top-left in framebuffer pixels
    float border; // outline width, px
    float fill;   // fill opacity inside the outline
    vec4 color;   // rgb; a = outline opacity
} pc;

void main() {
    vec2 p = gl_FragCoord.xy - pc.origin; // pixel centre, output-rect px
    vec2 lo = pc.rect.xy, hi = pc.rect.xy + pc.rect.zw;
    if (any(lessThan(p, lo)) || any(greaterThan(p, hi)))
        discard;
    float edge = min(min(p.x - lo.x, hi.x - p.x), min(p.y - lo.y, hi.y - p.y));
    if (edge < pc.border)
        outColor = vec4(pc.color.rgb, pc.color.a);
    else if (pc.fill > 0.0)
        outColor = vec4(pc.color.rgb, pc.fill);
    else
        discard;
}
