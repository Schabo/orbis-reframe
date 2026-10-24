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

// A view's pose: where it looks and how much it takes in.
struct Pose {
    float yaw = 0.0f;   // radians, about the vertical (Y) axis
    float pitch = 0.0f; // radians, about the horizontal (X) axis
    float roll = 0.0f;  // radians, about the view (Z) axis -- horizon tilt
    // The angle the picture spans corner to corner, radians: ~115 deg, what
    // 75 deg up and down takes in at 16:9. A double so that a vertical angle
    // turned into it (fovFor) comes back as the same float.
    double fov = 2.004501002893341;
    // The projection's d, 0 (rectilinear) to 1 (stereographic), or
    // kAutoDistortion.
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
    return (T(1) + distortion) * std::sin(angle) / (distortion + std::cos(angle));
}

// The plane's half-height for a vertical field of view (Lens::fovY).
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

// The d a pose renders with: its own, or Auto's for its field of view.
inline double distortionOf(const Pose& pose)
{
    return pose.distortionIsAuto() ? autoDistortion(pose.fov)
                                 : std::clamp(double(pose.distortion), 0.0, 1.0);
}

// The field of view's range, radians diagonal. The widest keeps the corners
// short of where the projection runs off to infinity (d + cos(theta) = 0):
// 145 deg rectilinear, 330 deg stereographic (a tiny planet with the sky
// round it). Auto: the widest stereographic, its distortion grows with the
// angle fast enough to stay within it.
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
// radius is the half-height times sqrt(1 + aspect^2).
template <class T>
inline T fovYFor(T fov, T distortion, T aspect)
{
    const T k = std::sqrt(T(1) + aspect * aspect);
    if (distortion <= T(0))
        return T(2) * std::atan(std::tan(fov * T(0.5)) / k);
    return T(2) * angleOfRadius(planeRadius(fov * T(0.5), distortion) / k, distortion);
}
template <class T>
inline T fovFor(T fovY, T distortion, T aspect)
{
    const T k = std::sqrt(T(1) + aspect * aspect);
    if (distortion <= T(0))
        return T(2) * std::atan(std::tan(fovY * T(0.5)) * k);
    return T(2) * angleOfRadius(planeRadius(fovY * T(0.5), distortion) * k, distortion);
}

// What a renderer draws `pose` with at `aspect` (width / height): the
// vertical angle through the centre (halfHeight's) and the d. The field of
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
