// The flat view's projection, the GLSL twin of src/core/ViewProjection.h
// (which explains it): a general perspective projection from (0, 0, d), d
// = 0 rectilinear, d = 1 stereographic. At d = 0 these are the plain
// rectilinear expressions, so the picture is unchanged to the bit. The view
// looks along -Z with +Y up; NDC y = -1 is the top.
//
// Both forms are computed and one is selected, rather than branched on d: a
// branch here changes how the driver compiles the rectilinear expressions
// (measured on NVIDIA), and with it the last bit of some pixels.

// The plane radius of a ray `angle` off the view axis; for the half-height,
// half the vertical field of view.
float viewPlaneRadius(float angle, float d)
{
    float rectilinear = tan(angle);
    float general = (1.0 + d) * sin(angle) / (d + cos(angle));
    return d <= 0.0 ? rectilinear : general;
}

// Image-plane point -> unit view ray: the line from the projection centre
// through (p, -1) meets the unit sphere on its far side.
vec3 viewPlaneToRay(vec2 p, float d)
{
    vec3 rectilinear = normalize(vec3(p, -1.0));
    vec3 w = vec3(p, -(1.0 + d));
    float ww = dot(w, w);
    float b = d * (1.0 + d);
    float s = (b + sqrt(max(b * b - ww * (d * d - 1.0), 0.0))) / ww;
    vec3 general = vec3(0.0, 0.0, d) + s * w;
    return d <= 0.0 ? rectilinear : general;
}

// The view ray of the pixel at `ndc` of a view with vertical field of view
// `fovY`, `aspect` (width / height) and distortion `d`.
vec3 viewRayAt(vec2 ndc, float fovY, float aspect, float d)
{
    float h = viewPlaneRadius(fovY * 0.5, d);
    return viewPlaneToRay(vec2(ndc.x * h * aspect, -ndc.y * h), d);
}
