#pragma once

// View <-> sphere geometry for subject tracking, in the conventions of
// equirect.frag / exportpipe::RenderRectilinear: a view ray
// v = (ndcX * tan(fovY/2) * aspect, -ndcY * tan(fovY/2), -1) (ndc y = -1 at the
// top) is rolled about Z, pitched about X, then yawed about Y into the
// *stabilized* direction q -- the frame ReframeTrack keys live in; the gyro
// de-rotation then takes q to the camera sphere. A pose (yaw, pitch) looks
// along dirFromYawPitch(yaw, pitch).
//
// Header-only and OpenCV-free, so the app can turn a viewport box into a
// tracking seed without the tracking library.

#include <algorithm>
#include <cmath>

namespace track {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

inline Vec3 normalized(const Vec3& v)
{
    const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return n > 0.0 ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{0.0, 0.0, -1.0};
}

inline double angleBetween(const Vec3& a, const Vec3& b)
{
    return std::acos(std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.0, 1.0));
}

// The view direction of pose (yaw, pitch): R(yaw, pitch) * (0, 0, -1).
inline Vec3 dirFromYawPitch(double yaw, double pitch)
{
    return {-std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};
}

inline void yawPitchFromDir(const Vec3& q, double& yaw, double& pitch)
{
    yaw = std::atan2(-q.x, -q.z);
    pitch = std::asin(std::clamp(q.y, -1.0, 1.0));
}

// Camera-frame view ray -> stabilized direction, through the pose.
inline Vec3 viewToStab(const Vec3& v, double yaw, double pitch, double roll = 0.0)
{
    const double cR = std::cos(roll), sR = std::sin(roll);
    const double rx = v.x * cR - v.y * sR, ry = v.x * sR + v.y * cR, rz = v.z;
    const double cP = std::cos(pitch), sP = std::sin(pitch);
    const double px = rx, py = ry * cP - rz * sP, pz = ry * sP + rz * cP;
    const double cY = std::cos(yaw), sY = std::sin(yaw);
    return {px * cY + pz * sY, py, -px * sY + pz * cY};
}

// Inverse of viewToStab (the transpose, roll last).
inline Vec3 stabToView(const Vec3& q, double yaw, double pitch, double roll = 0.0)
{
    const double cY = std::cos(yaw), sY = std::sin(yaw);
    const double px = q.x * cY - q.z * sY, pz = q.x * sY + q.z * cY, py = q.y;
    const double cP = std::cos(pitch), sP = std::sin(pitch);
    const double rx = px, ry = py * cP + pz * sP, rz = -py * sP + pz * cP;
    const double cR = std::cos(roll), sR = std::sin(roll);
    return {rx * cR + ry * sR, -rx * sR + ry * cR, rz};
}

// A point of a view at fractions (fx, fy) of its width / height (0,0 = top
// left) -> stabilized direction.
inline Vec3 viewPointToStab(double fx, double fy, double yaw, double pitch, double roll,
                            double fovY, double aspect)
{
    const double t = std::tan(fovY * 0.5);
    const Vec3 v = normalized({(fx * 2.0 - 1.0) * t * aspect, -(fy * 2.0 - 1.0) * t, -1.0});
    return viewToStab(v, yaw, pitch, roll);
}

// What to track: the subject's centre direction (stabilized) and its angular
// width / height, radians.
struct Seed {
    double yaw = 0.0, pitch = 0.0;
    double width = 0.0, height = 0.0;
};

// A box in a view (fractions of the view: x, y = top left, w, h) -> Seed.
inline Seed seedFromViewBox(double x, double y, double w, double h, double yaw, double pitch,
                            double roll, double fovY, double aspect)
{
    Seed s;
    yawPitchFromDir(viewPointToStab(x + w * 0.5, y + h * 0.5, yaw, pitch, roll, fovY, aspect),
                    s.yaw, s.pitch);
    const double cy = y + h * 0.5, cx = x + w * 0.5;
    s.width = angleBetween(viewPointToStab(x, cy, yaw, pitch, roll, fovY, aspect),
                           viewPointToStab(x + w, cy, yaw, pitch, roll, fovY, aspect));
    s.height = angleBetween(viewPointToStab(cx, y, yaw, pitch, roll, fovY, aspect),
                            viewPointToStab(cx, y + h, yaw, pitch, roll, fovY, aspect));
    return s;
}

} // namespace track
