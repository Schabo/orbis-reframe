#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

// The output-pixel -> view-space ray construction shared by every flat-view
// path, in plain C++: the port of src/render/shaders/view_ray.glsl (keep them
// in step -- preview, export, picking and outlines must agree). +Y up,
// -Z forward; the yaw/pitch/roll pose turns the ray, stab follows it.
// `proj` is ReframeTrack.h's Projection as an int (this header can't include
// it: track/TrackGeometry.h is included by core).

namespace viewproj {

constexpr double kPi = std::numbers::pi;

// NDC in [-1,1] -> the view-space ray, normalized. Returns false where the
// projection doesn't reach (outside the mirror ball, past a fisheye's rim):
// that pixel is black.
template <typename T>
bool viewRay(T ndcX, T ndcY, T fovY, T aspect, int proj, T out[3])
{
    const T x = ndcX * aspect;
    const T y = -ndcY;
    const T rho = std::sqrt(x * x + y * y);
    const T ax = rho > T(1e-6) ? x / rho : T(0);
    const T ay = rho > T(1e-6) ? y / rho : T(0);

    if (proj == 1) {
        // Stereographic ("tiny planet").
        const T edge = std::min(T(2) * fovY, T(kPi) - T(0.02));
        const T th = T(2) * std::atan(rho * std::tan(edge * T(0.5)));
        const T s = std::sin(th);
        out[0] = ax * s;
        out[1] = ay * s;
        out[2] = -std::cos(th);
        return true;
    }
    if (proj == 2) {
        // Equidistant fisheye.
        const T th = rho * fovY;
        const T s = std::sin(th);
        out[0] = ax * s;
        out[1] = ay * s;
        out[2] = -std::cos(th);
        return th <= T(kPi);
    }
    if (proj == 3) {
        // Mirror ball.
        const T br = std::min(fovY / T(1.75), T(1));
        const T ux = x / br, uy = y / br;
        const T r2 = ux * ux + uy * uy;
        if (r2 > T(1)) {
            out[0] = out[1] = T(0);
            out[2] = T(-1);
            return false;
        }
        const T nz = std::sqrt(T(1) - r2);
        out[0] = T(2) * ux * nz;
        out[1] = T(2) * uy * nz;
        out[2] = T(2) * nz * nz - T(1);
        return true;
    }
    if (proj == 4) {
        // Crystal ball: stereographic from the front pole -- centre shows
        // what is behind the view, the rim is the horizon.
        const T edge = std::min(T(2) * fovY, T(kPi) - T(0.02));
        const T th = T(2) * std::atan(rho * std::tan(edge * T(0.5)));
        const T s = std::sin(th);
        out[0] = ax * s;
        out[1] = ay * s;
        out[2] = std::cos(th);
        return true;
    }
    if (proj == 5) {
        // Panini ("natural view"), distance 1. Inverse of
        // x = 2 sin(l)/(1+cos(l)), y = 2 tan(p)/(1+cos(l)):
        // sin(l) = 4h/(h^2+4), cos(l) = (2s-h)/h, tan(p) = v(1+c)/2.
        const T t0 = std::tan(fovY * T(0.5));
        const T h = x * t0;
        const T v = y * t0;
        const T s = T(4) * h / (h * h + T(4));
        const T c = std::abs(h) > T(1e-6) ? (T(2) * s - h) / h : T(1);
        const T t = v * (T(1) + c) * T(0.5);
        const T inv = T(1) / std::sqrt(s * s + t * t + c * c);
        out[0] = s * inv;
        out[1] = t * inv;
        out[2] = -c * inv;
        return true;
    }
    if (proj == 6) {
        // Orthographic: the sphere as a globe from outside, scaled like the
        // mirror ball.
        const T br = std::min(fovY / T(1.75), T(1));
        const T ux = x / br, uy = y / br;
        const T r2 = ux * ux + uy * uy;
        if (r2 > T(1)) {
            out[0] = out[1] = T(0);
            out[2] = T(-1);
            return false;
        }
        out[0] = ux;
        out[1] = uy;
        out[2] = -std::sqrt(T(1) - r2);
        return true;
    }
    // Rectilinear: the plain pinhole ray.
    const T t = std::tan(fovY * T(0.5));
    const T inv = T(1) / std::sqrt(x * t * x * t + y * t * y * t + T(1));
    out[0] = x * t * inv;
    out[1] = y * t * inv;
    out[2] = -inv;
    return true;
}

// The inverse: a normalized view-space ray -> NDC in [-1,1]. Most
// projections cover the whole sphere, so there is no near-plane cull;
// rectilinear callers keep their own clip. Returns false for rectilinear
// rays at or behind the camera plane, and for orthographic rays on the
// ball's hidden back.
template <typename T>
bool viewNdc(const T v[3], T fovY, T aspect, int proj, T out[2])
{
    if (proj == 0) {
        if (v[2] >= T(0))
            return false;
        const T t = std::tan(fovY * T(0.5));
        out[0] = (v[0] / -v[2]) / (t * aspect);
        out[1] = -(v[1] / -v[2]) / t;
        return true;
    }
    if (proj == 4) {
        // Crystal ball: polar angle off +Z, otherwise the planet's inverse.
        const T edge = std::min(T(2) * fovY, T(kPi) - T(0.02));
        const T th = std::acos(std::clamp(v[2], T(-1), T(1)));
        const T s = std::sin(th);
        const T ax = s > T(1e-6) ? v[0] / s : T(0);
        const T ay = s > T(1e-6) ? v[1] / s : T(0);
        const T rho = std::tan(th * T(0.5)) / std::tan(edge * T(0.5));
        out[0] = ax * rho / aspect;
        out[1] = -ay * rho;
        return true;
    }
    if (proj == 5) {
        // Panini forward: longitude and latitude off -Z onto the plane.
        const T lam = std::atan2(v[0], -v[2]);
        const T cl = std::cos(lam);
        const T sl = std::sin(lam);
        const T tp = v[1] / std::sqrt(std::max(T(0), T(1) - v[1] * v[1]));
        const T t0 = std::tan(fovY * T(0.5));
        const T h = T(2) * sl / (T(1) + cl);
        const T k = T(2) * tp / (T(1) + cl);
        out[0] = h / (t0 * aspect);
        out[1] = -k / t0;
        return true;
    }
    if (proj == 6) {
        // Orthographic: only the facing hemisphere is on the ball.
        if (v[2] >= T(0))
            return false;
        const T br = std::min(fovY / T(1.75), T(1));
        out[0] = v[0] * br / aspect;
        out[1] = -v[1] * br;
        return true;
    }
    const T th = std::acos(std::clamp(-v[2], T(-1), T(1))); // polar angle off -Z
    const T s = std::sin(th);
    const T ax = s > T(1e-6) ? v[0] / s : T(0); // azimuth in the view
    const T ay = s > T(1e-6) ? v[1] / s : T(0);
    T rho;
    if (proj == 1) {
        const T edge = std::min(T(2) * fovY, T(kPi) - T(0.02));
        rho = std::tan(th * T(0.5)) / std::tan(edge * T(0.5));
    } else if (proj == 3) {
        // Invert the ball: normal from the mirrored ray, scaled to ndc.
        const T nz = std::sqrt(std::max(T(0), (v[2] + T(1)) * T(0.5)));
        const T br = std::min(fovY / T(1.75), T(1));
        out[0] = nz > T(1e-6) ? v[0] / (T(2) * nz) * br / aspect : T(0);
        out[1] = nz > T(1e-6) ? -(v[1] / (T(2) * nz)) * br : T(0);
        return true;
    } else { // proj == 2
        rho = th / fovY;
    }
    out[0] = ax * rho / aspect;
    out[1] = -ay * rho;
    return true;
}

} // namespace viewproj
