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
// Header-only, no Qt: the export (reframe_flatten) and the tracker use it
// too.

#include <cmath>

namespace view {

// A view's pose: where it looks and how much it takes in.
struct Pose {
    float yaw = 0.0f;   // radians, about the vertical (Y) axis
    float pitch = 0.0f; // radians, about the horizontal (X) axis
    float roll = 0.0f;  // radians, about the view (Z) axis -- horizon tilt
    // The angle the picture spans from its top edge to its bottom edge
    // through the centre, radians (~75 deg).
    float fovY = 1.309f;
    // The projection's d, 0 (rectilinear) to 1 (stereographic).
    float distortion = 0.0f;

    bool operator==(const Pose&) const = default;
};

// The plane radius of a ray `angle` radians off the view axis (r above).
// For the picture's half-height, pass half the vertical field of view.
template <class T>
inline T planeRadius(T angle, T distortion)
{
    if (distortion <= T(0))
        return std::tan(angle);
    return (T(1) + distortion) * std::sin(angle) / (distortion + std::cos(angle));
}

// The plane's half-height for `pose`'s vertical field of view.
template <class T>
inline T halfHeight(T fovY, T distortion)
{
    return planeRadius(fovY * T(0.5), distortion);
}

// Image-plane point (x, y) -> unit view ray `out` (x, y, z). The line from the
// projection centre (0, 0, d) through (x, y, -1) meets the unit sphere on its
// far side.
template <class T>
inline void planeToRay(T x, T y, T distortion, T out[3])
{
    if (distortion <= T(0)) {
        const T invLen = T(1) / std::sqrt(x * x + y * y + T(1));
        out[0] = x * invLen;
        out[1] = y * invLen;
        out[2] = -invLen;
        return;
    }
    const T d = distortion;
    const T wz = -(T(1) + d); // w = (x, y, -(1 + d)): centre -> plane point
    const T ww = x * x + y * y + wz * wz;
    const T b = d * (T(1) + d); // -(centre . w)
    const T s = (b + std::sqrt(b * b - ww * (d * d - T(1)))) / ww;
    out[0] = s * x;
    out[1] = s * y;
    out[2] = d + s * wz;
}

// The ray of the pixel at NDC (ndcX, ndcY) of a view with half-height
// `h` (halfHeight()) and `aspect` (width / height).
template <class T>
inline void ndcToRay(T ndcX, T ndcY, T h, T aspect, T distortion, T out[3])
{
    planeToRay(ndcX * h * aspect, -ndcY * h, distortion, out);
}

// A view-space point (any length; a direction or a point on a chord between
// two) -> its image-plane point, the central projection from (0, 0, d).
// False when it isn't in front of the projection centre (z >= d - `near`);
// a straight 3D segment maps to a straight segment in the plane, so a
// polygon clipped against z = d - near projects edge by edge.
template <class T>
inline bool pointToPlane(const T p[3], T distortion, T& x, T& y, T near = T(0))
{
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

} // namespace view
