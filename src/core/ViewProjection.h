#pragma once

// The flat view of the sphere: a pose, and the projection that turns its
// pixels into rays and back. Every implementation of the view -- the
// viewport's shaders, the GPU and CPU exports, picking in the viewport, the
// region outlines, the subject tracker's geometry -- goes through this
// header or its GLSL twin, src/render/shaders/view_projection.glsl, so they
// show the same picture.
//
// Conventions (equirect.frag's): the view looks along -Z with +Y up; a pixel
// at NDC (x, y) (y = -1 at the top) is the image-plane point
// (x * h * aspect, -y * h) at z = -1, where h is the plane's half-height.
//
// The projection is a general perspective one: the rays are cast from a
// projection centre `distortion` behind the sphere's centre, (0, 0, d), and
// the image is scaled so the picture's centre is magnified the same for
// every d. A ray at angle theta off the view axis lands at plane radius
//     r = (1 + d) sin(theta) / (d + cos(theta)),
// so d = 0 is rectilinear (tan(theta): straight lines stay straight, the
// edges stretch 1 / cos^2(theta)) and d = 1 is stereographic
// (2 tan(theta / 2): shapes keep their proportions, lines off the centre
// bow; very wide, the ground curls into a small planet). In between, the
// stretch at the edges eases as d grows. A ray is in the picture while
// d + cos(theta) > 0. At d = 0 every function here reduces to the plain
// rectilinear expressions, operation for operation, so the picture is
// unchanged to the bit.
//
// Past d = 1 the projection centre leaves the sphere: the globe, the sphere
// as a ball seen from outside. The world fits in a disc of plane radius
// sqrt((d + 1) / (d - 1)), out to acos(-1 / d) off the axis, and the plane
// beyond it has no ray (black); d -> infinity is orthographic, a hemisphere
// in a disc. The ray kept is still the far one, so the picture isn't
// mirrored and runs on from d = 1. What a pose stores, and every function
// here takes, is the amount `a` from 0 to 2: d = a up to 1, so those
// pictures are the same to the bit, and d = 1 / (2 - a) above it, worked
// in e = 2 - a = 1 / d, which keeps the far end exact and the float
// arithmetic from cancelling. A globe's size is taken as at d = 1: its
// field of view makes the picture as big as the tiny planet of that angle
// (halfHeight, fovYFor, maxFov).
//
// A pose gives its field of view as the diagonal angle, corner to corner,
// so a landscape and a portrait frame take in alike, and its distortion
// either fixed or Auto (following the field of view: none for a normal view,
// stereographic for a very wide one). What a renderer draws with is lens():
// the vertical angle through the centre and the d, for the aspect it draws
// at -- the picture's, or its pane's.
//
// Header-only, no Qt: the export (reframe_flatten) and the tracker use it
// too.

#include <algorithm>
#include <cmath>

namespace view {

// The distortion of a pose that follows its field of view (autoDistortion).
inline constexpr float kAutoDistortion = -1.0f;
// The largest distortion amount: the globe's far end, orthographic.
inline constexpr float kMaxDistortion = 2.0f;

// A view's pose: where it looks and how much it takes in.
struct Pose {
    float yaw = 0.0f;   // radians, about the vertical (Y) axis
    float pitch = 0.0f; // radians, about the horizontal (X) axis
    float roll = 0.0f;  // radians, about the view (Z) axis -- horizon tilt
    // The angle the picture spans corner to corner, radians: ~115 deg, what
    // 75 deg up and down takes in at 16:9. A double so that a vertical angle
    // turned into it (fovFor) comes back as the same float.
    double fov = 2.004501002893341;
    // The distortion amount: 0 (rectilinear) to 1 (stereographic) to 2 (the
    // globe's orthographic end), or kAutoDistortion.
    float distortion = kAutoDistortion;

    bool distortionIsAuto() const { return distortion < 0.0f; }
    bool operator==(const Pose&) const = default;
};

// The plane radius of a ray `angle` radians off the view axis (r above).
// For the picture's half-height, pass half the vertical field of view
// (Lens::fovY).
template <class T>
inline T planeRadius(T angle, T distortion)
{
    if (distortion <= T(0))
        return std::tan(angle);
    if (distortion > T(1)) {
        const T e = T(2) - distortion; // 1 / d
        return (T(1) + e) * std::sin(angle) / (T(1) + e * std::cos(angle));
    }
    return (T(1) + distortion) * std::sin(angle) / (distortion + std::cos(angle));
}

// The plane's half-height for a vertical field of view (Lens::fovY); a
// globe's as at d = 1.
template <class T>
inline T halfHeight(T fovY, T distortion)
{
    return planeRadius(fovY * T(0.5), std::min(distortion, T(1)));
}

// The globe's disc: the largest z a point of the unit sphere can have and
// still be seen, where the rays from the projection centre touch the sphere
// (z = 1 / d); for d <= 1, the centre's own plane, z = d. A polygon clipped
// against it projects edge by edge (pointToPlane).
template <class T>
inline T frontLimit(T distortion)
{
    return distortion > T(1) ? T(2) - distortion : distortion;
}

// Image-plane point (x, y) -> unit view ray `out` (x, y, z). The line from the
// projection centre (0, 0, d) through (x, y, -1) meets the unit sphere on its
// far side. False outside a globe's disc, where the line misses the sphere;
// `out` is then the ray at the disc's rim in that direction.
template <class T>
inline bool planeToRay(T x, T y, T distortion, T out[3])
{
    if (distortion <= T(0)) {
        const T invLen = T(1) / std::sqrt(x * x + y * y + T(1));
        out[0] = x * invLen;
        out[1] = y * invLen;
        out[2] = -invLen;
        return true;
    }
    if (distortion > T(1)) {
        // The same line scaled by e = 1 / d: P = (u x, u y, z) with u the far
        // root of ww u^2 - 2 (1 + e) u + (1 - e^2) = 0.
        const T e = T(2) - distortion;
        const T rr = x * x + y * y;
        const T k = (T(1) + e) * (T(1) + e) - rr * (T(1) - e * e);
        const T sk = std::sqrt(std::max(k, T(0)));
        const T ww = e * e * rr + (T(1) + e) * (T(1) + e);
        const T u = (T(1) + e + e * sk) / ww;
        out[0] = u * x;
        out[1] = u * y;
        out[2] = (e * rr - (T(1) + e) * sk) / ww;
        if (k >= T(0))
            return true;
        const T invLen = T(1) / std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        for (int i = 0; i < 3; ++i)
            out[i] *= invLen;
        return false;
    }
    const T d = distortion;
    const T wz = -(T(1) + d); // w = (x, y, -(1 + d)): centre -> plane point
    const T ww = x * x + y * y + wz * wz;
    const T b = d * (T(1) + d); // -(centre . w)
    const T s = (b + std::sqrt(b * b - ww * (d * d - T(1)))) / ww;
    out[0] = s * x;
    out[1] = s * y;
    out[2] = d + s * wz;
    return true;
}

// The ray of the pixel at NDC (ndcX, ndcY) of a view with half-height
// `h` (halfHeight()) and `aspect` (width / height); false outside a globe's
// disc (planeToRay).
template <class T>
inline bool ndcToRay(T ndcX, T ndcY, T h, T aspect, T distortion, T out[3])
{
    return planeToRay(ndcX * h * aspect, -ndcY * h, distortion, out);
}

// A view-space point (any length; a direction or a point on a chord between
// two) -> its image-plane point, the central projection from (0, 0, d).
// False when it isn't in front of the projection centre (z >= d - `near`);
// a straight 3D segment maps to a straight segment in the plane, so a
// polygon clipped against z = d - near projects edge by edge. On a globe
// the sphere's points past frontLimit are hidden behind its rim; that is the
// caller's to clip.
template <class T>
inline bool pointToPlane(const T p[3], T distortion, T& x, T& y, T near = T(0))
{
    if (distortion > T(1)) {
        const T e = T(2) - distortion; // the depth below is e (d - z)
        const T depth = T(1) - e * p[2];
        if (!(depth > e * near))
            return false;
        const T k = (T(1) + e) / depth;
        x = p[0] * k;
        y = p[1] * k;
        return true;
    }
    const T depth = distortion - p[2]; // distance in front of the centre
    if (!(depth > near))
        return false;
    if (distortion <= T(0)) {
        x = p[0] / depth;
        y = p[1] / depth;
        return true;
    }
    const T k = (T(1) + distortion) / depth;
    x = p[0] * k;
    y = p[1] * k;
    return true;
}

// A region outline -- a quad on the sphere whose edges are great-circle
// arcs, the privacy blur's shape -- as the view draws it: each edge cut
// into kOutlineSegments pieces along its arc, clipped to what the view
// sees, projected. The arcs bow in the picture wherever the projection
// isn't rectilinear, so straight lines between the projected corners
// wouldn't follow the blur.
inline constexpr int kOutlineSegments = 8;
// The most points outlineToPlane gives: the clip adds at most one point per
// edge it cuts.
inline constexpr int kOutlineMaxPoints = 8 * kOutlineSegments;

// `corner`: the quad's four corners in view space (directions, any length).
// Writes the outline's image-plane points to `out` (x, y pairs) and returns
// how many; fewer than 3 = nothing to draw. The points along each arc are
// clipped against z <= frontLimit - `near` (in front of the projection
// centre, not behind a globe's rim) as a polygon of short chords
// (Sutherland-Hodgman), so a cut outline closes along the picture's edge.
template <class T>
inline int outlineToPlane(const T corner[4][3], T distortion, T out[2 * kOutlineMaxPoints],
                          T near = T(1e-3))
{
    constexpr int kRing = 4 * kOutlineSegments;
    T ring[kRing][3];
    for (int e = 0; e < 4; ++e) {
        const T *a = corner[e], *b = corner[(e + 1) % 4];
        for (int s = 0; s < kOutlineSegments; ++s) {
            const T t = T(s) / T(kOutlineSegments);
            T *p = ring[e * kOutlineSegments + s];
            for (int k = 0; k < 3; ++k)
                p[k] = a[k] + t * (b[k] - a[k]);
            // Onto the sphere: the chord's point -> the arc's.
            const T len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (len > T(0))
                for (int k = 0; k < 3; ++k)
                    p[k] /= len;
        }
    }
    const T zMax = frontLimit(distortion) - near;
    int n = 0;
    for (int i = 0; i < kRing && n < kOutlineMaxPoints; ++i) {
        const T *a = ring[i], *b = ring[(i + 1) % kRing];
        const bool ina = a[2] <= zMax, inb = b[2] <= zMax;
        T c[3];
        if (ina) {
            pointToPlane(a, distortion, out[n * 2], out[n * 2 + 1]);
            ++n;
        }
        if (ina != inb && n < kOutlineMaxPoints) {
            const T t = (zMax - a[2]) / (b[2] - a[2]);
            for (int k = 0; k < 3; ++k)
                c[k] = a[k] + t * (b[k] - a[k]);
            pointToPlane(c, distortion, out[n * 2], out[n * 2 + 1]);
            ++n;
        }
    }
    return n;
}

// How much Auto lets a shape in the picture's corner stretch: the radial
// over the tangential magnification there, (1 + d cos) / (d + cos) at the
// corner's angle off the axis (1 / cos rectilinear, 1 stereographic).
// Chosen by eye on riding clips with straight lines: less bends lines
// that a little stretch wouldn't spoil, more smears the corners.
inline constexpr double kAutoCornerStretch = 1.5;

// The distortion Auto gives a diagonal field of view `fov`: the least that
// keeps the corners' stretch at kAutoCornerStretch, so lines stay as
// straight as that allows -- none up to ~96 deg, 20 % at the default 115,
// ~46 % at 145, ~75 % at 200. That only nears stereographic at 360, so from
// 270 deg, where the view is a tiny planet with no straight lines left to
// keep, it eases on to full at 300 and stays there, which lets Auto go as
// wide as stereographic does.
template <class T>
inline T autoDistortion(T fov)
{
    constexpr T s = T(kAutoCornerStretch);
    constexpr T kPlanetFrom = T(4.71238898038469); // 270 deg
    constexpr T kPlanetTo = T(5.235987755982989);  // 300 deg
    const T c = std::cos(fov * T(0.5));
    const T d = std::clamp((T(1) - s * c) / (s - c), T(0), T(1));
    const T t = std::clamp((fov - kPlanetFrom) / (kPlanetTo - kPlanetFrom), T(0), T(1));
    return d + (T(1) - d) * (t * t * (T(3) - T(2) * t));
}

// The distortion amount a pose renders with: its own, or Auto's for its
// field of view (never a globe: zooming out alone doesn't turn the picture
// into a disc on black).
inline double distortionOf(const Pose& pose)
{
    return pose.distortionIsAuto()
               ? autoDistortion(pose.fov)
               : std::clamp(double(pose.distortion), 0.0, double(kMaxDistortion));
}

// The field of view's range, radians diagonal. The widest keeps the corners
// short of where the projection runs off to infinity (d + cos(theta) = 0):
// 145 deg rectilinear, 330 deg stereographic (a tiny planet with the sky
// round it). Auto: the widest stereographic, its distortion grows with the
// angle fast enough to stay within it. A globe: as stereographic, its size
// is taken at d = 1.
inline constexpr double kMinFov = 0.3490658503988659; // 20 deg
template <class T>
inline T maxFov(T distortion)
{
    const T d = std::clamp(distortion, T(0), T(1));
    const T margin = T(0.30543261909900765) - T(0.04363323129985824) * d; // 17.5 .. 15 deg
    return T(2) * (std::acos(-d) - margin);
}
inline double maxFov(const Pose& pose)
{
    return maxFov(pose.distortionIsAuto() ? 1.0 : double(pose.distortion));
}

// The ray angle off the axis that lands at plane radius `r`, planeRadius's
// inverse.
template <class T>
inline T angleOfRadius(T r, T distortion)
{
    if (distortion <= T(0))
        return std::atan(r);
    T ray[3];
    planeToRay(r, T(0), distortion, ray);
    return std::atan2(ray[0], -ray[2]);
}

// The vertical angle through the centre of a picture `aspect` (width /
// height) wide whose diagonal angle is `fov`, and back. Its corner's plane
// radius is the half-height times sqrt(1 + aspect^2). A globe's angles are
// taken at d = 1, as its size is.
template <class T>
inline T fovYFor(T fov, T distortion, T aspect)
{
    distortion = std::min(distortion, T(1));
    const T k = std::sqrt(T(1) + aspect * aspect);
    if (distortion <= T(0))
        return T(2) * std::atan(std::tan(fov * T(0.5)) / k);
    return T(2) * angleOfRadius(planeRadius(fov * T(0.5), distortion) / k, distortion);
}
template <class T>
inline T fovFor(T fovY, T distortion, T aspect)
{
    distortion = std::min(distortion, T(1));
    const T k = std::sqrt(T(1) + aspect * aspect);
    if (distortion <= T(0))
        return T(2) * std::atan(std::tan(fovY * T(0.5)) * k);
    return T(2) * angleOfRadius(planeRadius(fovY * T(0.5), distortion) * k, distortion);
}

// What a renderer draws `pose` with at `aspect` (width / height): the
// vertical angle through the centre (halfHeight's) and the distortion
// amount. The field of
// view is held in its range for that d -- between two keys with different
// distortions the blend can reach past it.
struct Lens {
    float fovY = 1.309f;
    float distortion = 0.0f;
};
inline Lens lens(const Pose& pose, double aspect)
{
    if (!(aspect > 0.0))
        aspect = 16.0 / 9.0;
    const double d = distortionOf(pose);
    const double fov = std::clamp(pose.fov, kMinFov, maxFov(d));
    return {static_cast<float>(fovYFor(fov, d, aspect)), static_cast<float>(d)};
}

} // namespace view
