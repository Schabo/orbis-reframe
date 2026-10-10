// SPDX-License-Identifier: MIT OR Apache-2.0
#include "BlurQuads.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace insv {

namespace {

double dot3(const double a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double cross2(double ax, double ay, double bx, double by)
{
    return ax * by - ay * bx;
}

} // namespace

bool prepareBlurQuad(const BlurQuad& q, BlurQuadFrame& out)
{
    double c[3] = {0, 0, 0};
    for (const auto& k : q.corner)
        for (int i = 0; i < 3; ++i)
            c[i] += k[i];
    double n = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (n < 1e-9)
        return false;
    for (double& v : c)
        v /= n;
    // Tangent basis: r horizontal (to the right seen from the centre), u
    // perpendicular to it, pointing up. Any orthonormal pair would do.
    double r[3] = {-c[2], 0.0, c[0]};
    double rn = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    if (rn < 1e-6) { // looking straight up / down
        r[0] = 1.0;
        r[1] = r[2] = 0.0;
        rn = 1.0;
    }
    for (double& v : r)
        v /= rn;
    double u[3] = {c[1] * r[2] - c[2] * r[1], c[2] * r[0] - c[0] * r[2], c[0] * r[1] - c[1] * r[0]};
    if (u[1] < 0.0)
        for (double& v : u)
            v = -v;

    double minZ = 1.0;
    double px[4][2];
    for (int k = 0; k < 4; ++k) {
        const float* d = q.corner[k];
        const double z = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
        if (z < std::cos(80.0 * std::numbers::pi / 180.0))
            return false;
        minZ = std::min(minZ, z);
        px[k][0] = (r[0] * d[0] + r[1] * d[1] + r[2] * d[2]) / z;
        px[k][1] = (u[0] * d[0] + u[1] * d[1] + u[2] * d[2]) / z;
    }
    double scale = 0.0;
    for (const auto& p : px)
        scale = std::max(scale, std::hypot(p[0], p[1]));
    if (scale < 1e-9)
        return false;
    for (int k = 0; k < 4; ++k) {
        out.p[k][0] = static_cast<float>(px[k][0] / scale);
        out.p[k][1] = static_cast<float>(px[k][1] / scale);
    }
    // Area (shoelace) -- a collapsed quad has nothing to blur.
    double area = 0.0;
    for (int k = 0; k < 4; ++k)
        area += cross2(out.p[k][0], out.p[k][1], out.p[(k + 1) % 4][0], out.p[(k + 1) % 4][1]);
    if (std::abs(area) < 1e-6)
        return false;

    for (int i = 0; i < 3; ++i) {
        out.c[i] = static_cast<float>(c[i]);
        out.r[i] = static_cast<float>(r[i]);
        out.u[i] = static_cast<float>(u[i]);
    }
    // A little inside the farthest corner's angle, so the reject never cuts
    // the quad (float rounding in the shader).
    out.cosRadius = static_cast<float>(std::cos(std::min(std::acos(std::clamp(minZ, -1.0, 1.0)) * 1.02 + 1e-4,
                                                         std::numbers::pi / 2)));
    out.scale = static_cast<float>(scale);

    auto len = [&](int a, int b) {
        return std::hypot(double(out.p[b][0]) - out.p[a][0], double(out.p[b][1]) - out.p[a][1]);
    };
    const double ls = 0.5 * (len(0, 1) + len(3, 2)); // along s
    const double lt = 0.5 * (len(0, 3) + len(1, 2)); // along t
    const double shortSide = std::max(std::min(ls, lt), 1e-9);
    out.cells[0] = static_cast<float>(
        std::clamp(std::lround(kBlurCellsShort * ls / shortSide), 1L, long(kBlurCellsMax)));
    out.cells[1] = static_cast<float>(
        std::clamp(std::lround(kBlurCellsShort * lt / shortSide), 1L, long(kBlurCellsMax)));
    return true;
}

// Inverse bilinear (after Inigo Quilez): p = a + s e + t f + s t g.
bool blurQuadST(const BlurQuadFrame& f, const double dir[3], double& s, double& t)
{
    const double z = dot3(dir, f.c);
    if (z < f.cosRadius || z <= 0.0)
        return false;
    const double px = dot3(dir, f.r) / z / f.scale;
    const double py = dot3(dir, f.u) / z / f.scale;
    const double ax = f.p[0][0], ay = f.p[0][1];
    const double ex = f.p[1][0] - ax, ey = f.p[1][1] - ay;
    const double fx = f.p[3][0] - ax, fy = f.p[3][1] - ay;
    const double gx = ax - f.p[1][0] + f.p[2][0] - f.p[3][0];
    const double gy = ay - f.p[1][1] + f.p[2][1] - f.p[3][1];
    const double hx = px - ax, hy = py - ay;
    const double k2 = cross2(gx, gy, fx, fy);
    const double k1 = cross2(ex, ey, fx, fy) + cross2(hx, hy, gx, gy);
    const double k0 = cross2(hx, hy, ex, ey);
    auto sFor = [&](double tv) {
        const double dx = ex + gx * tv, dy = ey + gy * tv;
        // the better-conditioned component
        return std::abs(dx) > std::abs(dy) ? (hx - fx * tv) / dx : (hy - fy * tv) / dy;
    };
    auto inside = [](double a, double b) { return a >= 0.0 && a <= 1.0 && b >= 0.0 && b <= 1.0; };
    if (std::abs(k2) < 1e-7) { // a parallelogram: linear in t
        if (std::abs(k1) < 1e-12)
            return false;
        t = -k0 / k1;
        s = sFor(t);
        return inside(s, t);
    }
    double w = k1 * k1 - 4.0 * k0 * k2;
    if (w < 0.0)
        return false;
    w = std::sqrt(w);
    const double ik2 = 0.5 / k2;
    t = (-k1 - w) * ik2;
    s = sFor(t);
    if (inside(s, t))
        return true;
    t = (-k1 + w) * ik2;
    s = sFor(t);
    return inside(s, t);
}

void blurQuadDir(const BlurQuadFrame& f, double s, double t, double dir[3])
{
    double p[2];
    for (int k = 0; k < 2; ++k) {
        const double top = f.p[0][k] + (f.p[1][k] - f.p[0][k]) * s;
        const double bot = f.p[3][k] + (f.p[2][k] - f.p[3][k]) * s;
        p[k] = (top + (bot - top) * t) * f.scale;
    }
    double n = 0.0;
    for (int i = 0; i < 3; ++i) {
        dir[i] = f.c[i] + f.r[i] * p[0] + f.u[i] * p[1];
        n += dir[i] * dir[i];
    }
    n = std::sqrt(n);
    for (int i = 0; i < 3; ++i)
        dir[i] /= n;
}

double blurQuadWeight(double s, double t)
{
    const double e = std::min(std::min(s, 1.0 - s), std::min(t, 1.0 - t));
    const double x = std::clamp(e / kBlurFeather, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x); // smoothstep, as the shader
}

void blurTapDir(const BlurQuadFrame& f, double s, double t, int i, int j, double dir[3])
{
    const double cs = std::min(std::floor(s * f.cells[0]), f.cells[0] - 1.0);
    const double ct = std::min(std::floor(t * f.cells[1]), f.cells[1] - 1.0);
    blurQuadDir(f, (cs + (i + 0.5) / kBlurTaps) / f.cells[0], (ct + (j + 0.5) / kBlurTaps) / f.cells[1],
                dir);
}

} // namespace insv
