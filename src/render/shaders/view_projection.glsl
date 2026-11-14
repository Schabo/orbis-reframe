// The flat view's projection, the GLSL twin of src/core/ViewProjection.h
// (which explains it): a general perspective projection from (0, 0, d), d
// = 0 rectilinear, d = 1 stereographic. At d = 0 these are the plain
// rectilinear expressions, so the picture is unchanged to the bit. The view
// looks along -Z with +Y up; NDC y = -1 is the top. `d` here is the
// distortion amount: past 1 it is the globe, d = 1 / (2 - amount), worked
// in e = 2 - amount, with no ray outside its disc (viewInPicture).
//
// Natural, the Panini amount p, and Fisheye, the amount f, are compiled in
// only with VIEW_SHAPE defined (a second build of each view shader, for
// views with p or f above 0): with Natural the rays land on the ellipsoid
// x^2 + (1 - p) y^2 + z^2 = 1 instead of the sphere; Fisheye turns them
// toward the fisheye's radial maps; viewRayAt / viewInPicture take p and
// f. Without it these are the sphere's alone: with Natural's expressions in
// the same shader -- selected or behind a branch on p -- the driver
// compiled the others differently, and the last bit of existing pictures
// changed.
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

// A globe's disc: (1 + e)^2 - r^2 (1 - e^2), negative where the line from the
// projection centre through the plane point misses the sphere.
float viewGlobeRoot(vec2 p, float d)
{
    float e = 2.0 - d;
    return (1.0 + e) * (1.0 + e) - dot(p, p) * (1.0 - e * e);
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
    // The globe: the same line scaled by e = 1 / d, its far root u.
    float e = 2.0 - d;
    float rr = dot(p, p);
    float sk = sqrt(max(viewGlobeRoot(p, d), 0.0));
    float gw = e * e * rr + (1.0 + e) * (1.0 + e);
    float u = (1.0 + e + e * sk) / gw;
    vec3 globe = vec3(u * p, (e * rr - (1.0 + e) * sk) / gw);
    return d <= 0.0 ? rectilinear : d > 1.0 ? globe : general;
}

#ifdef VIEW_SHAPE

// The half-height of a vertical field of view `fovY` on the ellipsoid (a
// globe's as at d = 1): the ray fovY / 2 up meets it at 1 / n of its length,
// n = sqrt(1 - p sin^2), and lands at (1 + d) sin / (d n + cos).
float naturalHalfHeight(float fovY, float d, float pn)
{
    float dd = min(d, 1.0);
    float a = fovY * 0.5;
    float n = sqrt(1.0 - pn * sin(a) * sin(a));
    float natural = (1.0 + dd) * sin(a) / (dd * n + cos(a));
    return dd <= 0.0 ? tan(a) : natural;
}

// A globe's disc on the ellipsoid: viewGlobeRoot's, with x^2 + q y^2 (q =
// 1 - p) for x^2 + y^2.
float naturalGlobeRoot(vec2 p, float d, float pn)
{
    float e = 2.0 - d;
    float rq = p.x * p.x + (1.0 - pn) * p.y * p.y;
    return (1.0 + e) * (1.0 + e) - rq * (1.0 - e * e);
}

// Image-plane point -> unit view ray on the ellipsoid: viewPlaneToRay's
// roots with x^2 + q y^2 for x^2 + y^2, then made unit length.
vec3 naturalPlaneToRay(vec2 p, float d, float pn)
{
    vec3 rectilinear = normalize(vec3(p, -1.0));
    float rq = p.x * p.x + (1.0 - pn) * p.y * p.y;
    vec3 w = vec3(p, -(1.0 + d));
    float ww = rq + w.z * w.z;
    float b = d * (1.0 + d);
    float s = (b + sqrt(max(b * b - ww * (d * d - 1.0), 0.0))) / ww;
    vec3 general = vec3(0.0, 0.0, d) + s * w;
    float e = 2.0 - d;
    float sk = sqrt(max(naturalGlobeRoot(p, d, pn), 0.0));
    float gw = e * e * rq + (1.0 + e) * (1.0 + e);
    float u = (1.0 + e + e * sk) / gw;
    vec3 globe = vec3(u * p, (e * rq - (1.0 + e) * sk) / gw);
    return d <= 0.0 ? rectilinear : normalize(d > 1.0 ? globe : general);
}

const float VIEW_PI = 3.14159265358979323846;

// Fisheye from 1 up, k = (f - 1) / 2: the angle off the axis of plane radius
// r, asin(k r) / k (r at k = 0).
float fisheyeAngle(float r, float f)
{
    float k = 0.5 * (f - 1.0);
    float bent = asin(min(k * r, 1.0)) / max(k, 1e-6);
    return k < 1e-6 ? r : bent;
}

// The angle off the axis Fisheye `f` shows at plane point `p`, and its
// azimuth: from 1 up the fisheye's; below, the base view's ray `b` turned
// toward the equidistant one (ViewProjection.h).
vec2 fisheyeAngles(vec2 p, vec3 b, float f)
{
    float r = length(p);
    vec2 pp = r > 0.0 ? p : vec2(1.0, 0.0);
    vec2 bb = dot(b.xy, b.xy) > 0.0 ? b.xy : pp;
    float thetaB = atan(length(b.xy), -b.z);
    float psi = atan(pp.y, pp.x);
    float psiB = atan(bb.y, bb.x);
    float turn = psi - psiB;
    turn -= 2.0 * VIEW_PI * round(turn / (2.0 * VIEW_PI));
    vec2 blended = vec2(mix(thetaB, r, f), psiB + f * turn);
    vec2 fish = vec2(fisheyeAngle(r, f), psi);
    return f >= 1.0 ? fish : blended;
}

// Image-plane point -> unit view ray with Natural `pn` and Fisheye `f`.
vec3 shapePlaneToRay(vec2 p, float d, float pn, float f)
{
    vec3 b = naturalPlaneToRay(p, d, pn);
    vec2 a = fisheyeAngles(p, b, f);
    float theta = min(a.x, VIEW_PI);
    vec3 fish = vec3(sin(theta) * cos(a.y), sin(theta) * sin(a.y), -cos(theta));
    return f <= 0.0 ? b : fish;
}

// Whether plane point `p` shows the world: everywhere but outside a globe's
// disc (with Fisheye below 1, scaled by 1 / (1 - f)), past the sphere's
// back, and outside the mirror ball's disc.
bool shapeInPicture(vec2 p, float d, float pn, float f)
{
    float g = f > 0.0 && f < 1.0 ? 1.0 - f : 1.0;
    bool globe = d <= 1.0 || naturalGlobeRoot(g * p, d, pn) >= 0.0;
    vec2 a = fisheyeAngles(p, naturalPlaneToRay(p, d, pn), f);
    float k = 0.5 * (f - 1.0);
    bool ball = f < 1.0 || k < 1e-6 || k * length(p) <= 1.0;
    bool fish = f >= 1.0 ? ball && a.x <= VIEW_PI : globe && a.x <= VIEW_PI;
    return f <= 0.0 ? globe : fish;
}

// The view ray of the pixel at `ndc` of a view with vertical field of view
// `fovY`, `aspect` (width / height), distortion `d`, Natural `pn` and
// Fisheye `f`. A globe's size is taken at d = 1; Fisheye leaves it alone.
vec3 viewRayAt(vec2 ndc, float fovY, float aspect, float d, float pn, float f)
{
    float h = naturalHalfHeight(fovY, d, pn);
    return shapePlaneToRay(vec2(ndc.x * h * aspect, -ndc.y * h), d, pn, f);
}

// Whether the pixel at `ndc` shows the world (shapeInPicture); the rest is
// black.
bool viewInPicture(vec2 ndc, float fovY, float aspect, float d, float pn, float f)
{
    float h = naturalHalfHeight(fovY, d, pn);
    return shapeInPicture(vec2(ndc.x * h * aspect, -ndc.y * h), d, pn, f);
}

#else

// The view ray of the pixel at `ndc` of a view with vertical field of view
// `fovY`, `aspect` (width / height) and distortion `d`.
// A globe's size is taken at d = 1.
vec3 viewRayAt(vec2 ndc, float fovY, float aspect, float d)
{
    float h = viewPlaneRadius(fovY * 0.5, min(d, 1.0));
    return viewPlaneToRay(vec2(ndc.x * h * aspect, -ndc.y * h), d);
}

// Whether the pixel at `ndc` shows the sphere: everywhere but outside a
// globe's disc, which is black.
bool viewInPicture(vec2 ndc, float fovY, float aspect, float d)
{
    float h = viewPlaneRadius(fovY * 0.5, min(d, 1.0));
    return d <= 1.0 || viewGlobeRoot(vec2(ndc.x * h * aspect, -ndc.y * h), d) >= 0.0;
}

#endif
