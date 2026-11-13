// ViewProjection.h: the flat view's general perspective projection -- the
// rectilinear and stereographic ends, rays and plane points round trip, the
// centre's magnification, what is in front of the projection centre; the
// diagonal field of view, the automatic distortion and the angle limits;
// the globe past d = 1; the region outlines.
// The exit code is the failure count.

#include "support/ModelTest.h"

#include "ViewProjection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace {

constexpr double kPi = std::numbers::pi;

bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps * std::max(1.0, std::abs(b));
}

void testEnds()
{
    for (double a = 0.0; a < 1.5; a += 0.1) {
        // d = 0 is rectilinear, d = 1 stereographic.
        CHECK(view::planeRadius(a, 0.0) == std::tan(a));
        CHECK(near(view::planeRadius(a, 1.0), 2.0 * std::tan(a / 2.0)));
    }
    // A half-height is the radius of half the field of view.
    CHECK(view::halfHeight(1.309f, 0.0f) == std::tan(1.309f * 0.5f));
    CHECK(near(view::halfHeight(2.0, 0.5), view::planeRadius(1.0, 0.5)));
}

// At d = 0 the ray is the plain rectilinear one, to the bit -- the picture
// of every existing project depends on it.
void testRectilinearExact()
{
    for (float x : {-1.7f, -0.3f, 0.0f, 0.42f, 1.9f})
        for (float y : {-1.1f, 0.0f, 0.77f}) {
            float r[3];
            view::planeToRay(x, y, 0.0f, r);
            const float invLen = 1.0f / std::sqrt(x * x + y * y + 1.0f);
            CHECK(r[0] == x * invLen && r[1] == y * invLen && r[2] == -invLen);
            float px = 0.0f, py = 0.0f;
            const float p[3] = {x, y, -1.0f};
            CHECK(view::pointToPlane(p, 0.0f, px, py) && px == x && py == y);
        }
}

void testRoundTrip()
{
    for (double d : {0.0, 0.1, 0.25, 0.5, 0.75, 1.0}) {
        // Every angle in front of the projection centre (d + cos > 0).
        const double limit = std::acos(-d) - 0.05;
        for (double theta = 0.0; theta < limit; theta += 0.07)
            for (double phi = -kPi; phi < kPi; phi += 0.5) {
                const double dir[3] = {std::sin(theta) * std::cos(phi),
                                       std::sin(theta) * std::sin(phi), -std::cos(theta)};
                double x = 0.0, y = 0.0;
                CHECK(view::pointToPlane(dir, d, x, y));
                // The plane radius is the formula's.
                CHECK(near(std::hypot(x, y), view::planeRadius(theta, d), 1e-9));
                double r[3];
                view::planeToRay(x, y, d, r);
                CHECK(near(r[0] * r[0] + r[1] * r[1] + r[2] * r[2], 1.0, 1e-12));
                CHECK(std::abs(r[0] - dir[0]) < 1e-9 && std::abs(r[1] - dir[1]) < 1e-9 &&
                      std::abs(r[2] - dir[2]) < 1e-9);
            }
    }
}

// The picture's centre is magnified the same for every d, so a change of d
// keeps what's in the middle the same size.
void testCentreScale()
{
    for (double d : {0.0, 0.3, 0.6, 1.0}) {
        const double a = 1e-6;
        CHECK(near(view::planeRadius(a, d) / a, 1.0, 1e-9));
        // ... and the edges stretch less as d grows.
        CHECK(view::planeRadius(1.0, d) <= std::tan(1.0) + 1e-12);
    }
    CHECK(view::planeRadius(1.0, 0.5) < view::planeRadius(1.0, 0.0));
    CHECK(view::planeRadius(1.0, 1.0) < view::planeRadius(1.0, 0.5));
}

void testInFront()
{
    // Straight behind the view is never in the picture; at d = 1 everything
    // but that is.
    const double back[3] = {0.0, 0.0, 1.0};
    double x = 0.0, y = 0.0;
    CHECK(!view::pointToPlane(back, 1.0, x, y));
    CHECK(!view::pointToPlane(back, 0.0, x, y));
    const double side[3] = {1.0, 0.0, 0.0}; // 90 degrees off the axis
    CHECK(!view::pointToPlane(side, 0.0, x, y));
    CHECK(view::pointToPlane(side, 0.5, x, y) && near(x, 3.0)); // (1 + d) / d
    const double behind[3] = {0.0, 0.6, 0.8}; // ~143 degrees off the axis
    CHECK(!view::pointToPlane(behind, 0.5, x, y));
    CHECK(view::pointToPlane(behind, 1.0, x, y));
    // `near` keeps a margin in front of the centre.
    const double close[3] = {0.0, 0.0, -0.0005};
    CHECK(view::pointToPlane(close, 0.0, x, y));
    CHECK(!view::pointToPlane(close, 0.0, x, y, 1e-3));
}

void testNdc()
{
    // The NDC's corners and centre, the y axis pointing down.
    double r[3];
    view::ndcToRay(0.0, 0.0, 0.7, 16.0 / 9.0, 0.4, r);
    CHECK(near(r[2], -1.0) && std::abs(r[0]) < 1e-12 && std::abs(r[1]) < 1e-12);
    view::ndcToRay(0.0, -1.0, view::halfHeight(1.2, 0.4), 1.5, 0.4, r);
    CHECK(r[1] > 0.0 && near(std::acos(-r[2]), 0.6, 1e-9)); // the top edge, half the FOV up
    view::ndcToRay(1.0, 0.0, 0.5, 2.0, 0.0, r);
    CHECK(r[0] > 0.0 && near(r[0] / -r[2], 1.0));
}

void testPose()
{
    const view::Pose p;
    CHECK(p.distortionIsAuto() && p.yaw == 0.0f);
    // The default field of view frames 75 degrees up and down at 16:9.
    CHECK(float(view::fovYFor(p.fov, 0.0, 16.0 / 9.0)) == 1.309f);
    view::Pose q = p;
    CHECK(q == p);
    q.distortion = 0.5f;
    CHECK(!(q == p));
}

// The diagonal angle and the vertical one through the centre: the corner's
// plane radius is the half-height times sqrt(1 + aspect^2).
void testDiagonal()
{
    for (double aspect : {16.0 / 9.0, 9.0 / 16.0, 1.0, 4.0 / 3.0, 0.3})
        for (double d : {0.0, 0.3, 0.7, 1.0})
            for (double fov = view::kMinFov; fov <= view::maxFov(d); fov += 0.05) {
                const double fovY = view::fovYFor(fov, d, aspect);
                CHECK(near(view::fovFor(fovY, d, aspect), fov, 1e-12));
                CHECK(near(view::halfHeight(fovY, d) * std::sqrt(1.0 + aspect * aspect),
                           view::planeRadius(fov / 2.0, d), 1e-9));
                CHECK(fovY < fov); // the corners are further out than the top edge
            }
    // At d = 0, the plain rectilinear relation.
    CHECK(near(view::fovYFor(2.0, 0.0, 16.0 / 9.0),
               2.0 * std::atan(std::tan(1.0) / std::sqrt(1.0 + 256.0 / 81.0))));
    for (double r : {0.1, 1.0, 3.0, 20.0})
        for (double d : {0.0, 0.5, 1.0})
            CHECK(near(view::planeRadius(view::angleOfRadius(r, d), d), r, 1e-9));
}

// An older project's vertical angle, turned into the diagonal, comes back to
// the same float at every render site: its picture at d = 0 is the bit-exact
// one it had.
void testOldVerticalExact()
{
    const double aspects[] = {16.0 / 9.0, 9.0 / 16.0, 1.0, 4.0 / 3.0, 1916.0 / 538.0};
    uint32_t state = 12345;
    for (int i = 0; i < 200000; ++i) {
        state = state * 1664525u + 1013904223u;
        const float fovY = 0.35f + 1.57f * float(state >> 8) / float(1u << 24);
        const double aspect = aspects[i % 5];
        view::Pose p;
        p.fov = view::fovFor<double>(fovY, 0.0, aspect);
        p.distortion = 0.0f;
        if (p.fov > view::maxFov(0.0)) // a very wide pane at 110 deg up and down
            continue;
        const view::Lens lens = view::lens(p, aspect);
        CHECK(lens.fovY == fovY && lens.distortion == 0.0f);
    }
}

void testAutoDistortion()
{
    constexpr double deg = kPi / 180.0;
    CHECK(view::autoDistortion(20.0 * deg) == 0.0);
    CHECK(view::autoDistortion(90.0 * deg) == 0.0);
    double last = 0.0;
    for (double fov = 20.0; fov < 360.0; fov += 1.0) {
        const double d = view::autoDistortion(fov * deg);
        CHECK(d >= last && d >= 0.0 && d <= 1.0);
        last = d;
        if (fov > 270.0)
            continue; // a tiny planet: on to stereographic
        // The corner's shape stretch: rectilinear's where that is under the
        // limit, else held at the limit.
        const double c = std::cos(fov * deg / 2.0);
        const double stretch = (1.0 + d * c) / (d + c);
        if (d > 0.0)
            CHECK(near(stretch, view::kAutoCornerStretch, 1e-9));
        else
            CHECK(c <= 0.0 || 1.0 / c <= view::kAutoCornerStretch + 1e-12);
    }
    CHECK(view::autoDistortion(300.0 * deg) == 1.0);
    CHECK(view::autoDistortion(330.0 * deg) == 1.0);
    view::Pose p;
    p.fov = 145.0 * deg;
    CHECK(near(view::distortionOf(p), 0.4577, 1e-4));
    p.distortion = 0.25f;
    CHECK(view::distortionOf(p) == 0.25);
}

void testLimits()
{
    constexpr double deg = kPi / 180.0;
    CHECK(near(view::maxFov(0.0), 145.0 * deg));
    CHECK(near(view::maxFov(1.0), 330.0 * deg));
    for (double d = 0.0; d <= 1.0; d += 0.05) {
        CHECK(view::maxFov(d + 0.05) > view::maxFov(d));
        // The corners stay in front of the projection centre, with room.
        CHECK(d + std::cos(view::maxFov(d) / 2.0) > 0.03);
    }
    // Auto's distortion grows with the angle fast enough: every angle up to
    // its limit is within the limit of the distortion it gets.
    view::Pose a;
    CHECK(near(view::maxFov(a), 330.0 * deg));
    for (double fov = view::kMinFov; fov <= view::maxFov(a); fov += 0.01)
        CHECK(fov <= view::maxFov(view::autoDistortion(fov)) + 1e-12);
    // A renderer holds the angle in its distortion's range.
    view::Pose p;
    p.fov = 300.0 * deg;
    p.distortion = 0.0f;
    CHECK(near(view::lens(p, 1.0).fovY, view::fovYFor(view::maxFov(0.0), 0.0, 1.0), 1e-6));
    p.fov = 0.01;
    CHECK(near(view::lens(p, 1.0).fovY, view::fovYFor(view::kMinFov, 0.0, 1.0), 1e-6));
}

// The globe, amounts past 1: d = 1 / (2 - a). The world fits a disc of plane
// radius sqrt((d + 1) / (d - 1)), out to acos(-1 / d) off the axis; beyond
// it no ray. It runs on from stereographic and ends orthographic, and its
// size and angles are taken at d = 1.
void testGlobe()
{
    constexpr double deg = kPi / 180.0;
    for (double a : {1.2, 1.5, 1.8, 1.95, 2.0}) {
        const double e = 2.0 - a;
        // Directions out to just inside the rim round trip, as unit rays.
        const double rim = std::acos(-e);
        for (double t = 0.0; t < rim - 1e-3; t += 0.05) {
            for (double phi = 0.0; phi < 2.0 * kPi; phi += 0.7) {
                const double dir[3] = {std::sin(t) * std::cos(phi), std::sin(t) * std::sin(phi),
                                       -std::cos(t)};
                CHECK(dir[2] < view::frontLimit(a));
                double x = 0.0, y = 0.0, r[3];
                CHECK(view::pointToPlane(dir, a, x, y));
                CHECK(near(std::hypot(x, y), view::planeRadius(t, a), 1e-9));
                CHECK(view::planeToRay(x, y, a, r));
                CHECK(near(r[0], dir[0], 1e-7) && near(r[1], dir[1], 1e-7) &&
                      near(r[2], dir[2], 1e-7));
            }
        }
        // The disc's rim, and black past it with the rim's ray, unit length.
        const double disc = e > 0.0 ? std::sqrt((1.0 + e) / (1.0 - e)) : 1.0;
        double r[3];
        CHECK(view::planeToRay(disc * 0.999, 0.0, a, r));
        CHECK(!view::planeToRay(disc * 1.001, 0.0, a, r));
        CHECK(near(std::hypot(r[0], r[1], r[2]), 1.0, 1e-12));
        CHECK(near(r[2], e, 1e-2) && r[0] > 0.0);
        CHECK(!view::planeToRay(0.0, -5.0, a, r) && r[1] < 0.0);
        // The centre is magnified as at every d.
        CHECK(near(view::planeRadius(1e-6, a), 1e-6, 1e-6));
    }
    // Orthographic at 2: the plane point is the ray's x and y.
    double r[3];
    CHECK(view::planeToRay(0.6, -0.3, 2.0, r));
    CHECK(near(r[0], 0.6, 1e-15) && near(r[1], -0.3, 1e-15) &&
          near(r[2], -std::sqrt(1.0 - 0.45), 1e-15));
    // On from stereographic: the rays and radii on both sides of 1 meet.
    for (double x : {0.3, 1.0, 2.5, 6.0}) {
        double lo[3], hi[3];
        view::planeToRay(x, 0.4, 1.0, lo);
        view::planeToRay(x, 0.4, 1.0 + 1e-9, hi);
        for (int i = 0; i < 3; ++i)
            CHECK(near(lo[i], hi[i], 1e-7));
    }
    CHECK(near(view::planeRadius(1.2, 1.0), view::planeRadius(1.2, 1.0 + 1e-12), 1e-9));
    CHECK(view::frontLimit(1.0) == 1.0 && view::frontLimit(1.5) == 0.5 &&
          view::frontLimit(0.3) == 0.3);
    // The size and the angles are the tiny planet's.
    CHECK(view::halfHeight(1.4, 1.7) == view::halfHeight(1.4, 1.0));
    CHECK(view::fovYFor(4.0, 1.7, 16.0 / 9.0) == view::fovYFor(4.0, 1.0, 16.0 / 9.0));
    CHECK(view::fovFor(2.0, 1.7, 16.0 / 9.0) == view::fovFor(2.0, 1.0, 16.0 / 9.0));
    CHECK(view::maxFov(1.7) == view::maxFov(1.0));
    view::Pose p;
    p.fov = 200.0 * deg;
    p.distortion = 1.7f;
    CHECK(near(view::distortionOf(p), 1.7, 1e-7));
    const view::Lens lens = view::lens(p, 16.0 / 9.0);
    CHECK(lens.distortion == 1.7f);
    p.distortion = 1.0f;
    CHECK(lens.fovY == view::lens(p, 16.0 / 9.0).fovY);
    p.distortion = 2.5f;
    CHECK(view::distortionOf(p) == 2.0);
}

// The distance from (x, y) to the segment a-b in the plane.
double segmentDistance(double x, double y, const double a[2], const double b[2])
{
    const double ex = b[0] - a[0], ey = b[1] - a[1];
    const double t = std::clamp(((x - a[0]) * ex + (y - a[1]) * ey) / (ex * ex + ey * ey), 0.0, 1.0);
    return std::hypot(x - a[0] - t * ex, y - a[1] - t * ey);
}

void testOutline()
{
    constexpr double deg = kPi / 180.0;
    // A quad ~40 deg wide round the view's centre, corners off the unit
    // sphere's radius (the arcs don't depend on it).
    auto dir = [](double az, double el, double len, double out[3]) {
        out[0] = len * std::cos(el) * std::sin(az);
        out[1] = len * std::sin(el);
        out[2] = -len * std::cos(el) * std::cos(az);
    };
    double quad[4][3];
    dir(-20 * deg, 15 * deg, 1.0, quad[0]);
    dir(25 * deg, 12 * deg, 2.0, quad[1]);
    dir(20 * deg, -18 * deg, 0.5, quad[2]);
    dir(-22 * deg, -10 * deg, 1.0, quad[3]);
    double pts[2 * view::kOutlineMaxPoints];

    // Rectilinear: the arcs are the straight lines between the corners, the
    // picture the outline had as a plain quad.
    int n = view::outlineToPlane(quad, 0.0, pts);
    CHECK(n == 4 * view::kOutlineSegments);
    double c[4][2];
    for (int k = 0; k < 4; ++k) {
        CHECK(view::pointToPlane(quad[k], 0.0, c[k][0], c[k][1]));
        const double *p = pts + 2 * k * view::kOutlineSegments;
        CHECK(near(p[0], c[k][0], 1e-12) && near(p[1], c[k][1], 1e-12));
    }
    for (int i = 0; i < n; ++i) {
        const int k = i / view::kOutlineSegments;
        CHECK(segmentDistance(pts[2 * i], pts[2 * i + 1], c[k], c[(k + 1) % 4]) < 1e-12);
    }

    // Every point is on its edge's great circle, at every distortion: its
    // ray lies in the plane through the centre and the edge's corners.
    for (double a : {0.0, 0.4, 1.0, 1.5, 2.0}) {
        n = view::outlineToPlane(quad, a, pts);
        CHECK(n == 4 * view::kOutlineSegments);
        for (int i = 0; i < n; ++i) {
            const int k = i / view::kOutlineSegments;
            const double *p = quad[k], *q = quad[(k + 1) % 4];
            double nrm[3] = {p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2],
                             p[0] * q[1] - p[1] * q[0]};
            const double len = std::hypot(nrm[0], nrm[1], nrm[2]);
            double r[3];
            CHECK(view::planeToRay(pts[2 * i], pts[2 * i + 1], a, r));
            CHECK(std::abs(r[0] * nrm[0] + r[1] * nrm[1] + r[2] * nrm[2]) / len < 1e-9);
        }
    }
    // ... and bows away from the corners' straight lines once d > 0: the
    // middle of the top edge, off the chord.
    n = view::outlineToPlane(quad, 1.0, pts);
    {
        double a0[2], a1[2];
        view::pointToPlane(quad[0], 1.0, a0[0], a0[1]);
        view::pointToPlane(quad[1], 1.0, a1[0], a1[1]);
        const double *m = pts + 2 * (view::kOutlineSegments / 2);
        CHECK(segmentDistance(m[0], m[1], a0, a1) > 1e-3);
    }

    // Half of it behind the view: cut at the plane in front of the
    // projection centre, the cut closing the polygon; no point past it.
    double wide[4][3];
    dir(-60 * deg, 20 * deg, 1.0, wide[0]);
    dir(150 * deg, 20 * deg, 1.0, wide[1]);
    dir(150 * deg, -20 * deg, 1.0, wide[2]);
    dir(-60 * deg, -20 * deg, 1.0, wide[3]);
    for (double a : {0.0, 0.5, 1.0, 1.6}) {
        n = view::outlineToPlane(wide, a, pts);
        CHECK(n >= 3 && n <= view::kOutlineMaxPoints);
        for (int i = 0; i < n; ++i) {
            CHECK(std::isfinite(pts[2 * i]) && std::isfinite(pts[2 * i + 1]));
        }
    }
    // All of it behind: nothing.
    double back[4][3];
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i)
            back[k][i] = -quad[k][i];
    CHECK(view::outlineToPlane(back, 0.0, pts) == 0);
    CHECK(view::outlineToPlane(back, 1.5, pts) == 0);
}

} // namespace

int main()
{
    testEnds();
    testRectilinearExact();
    testRoundTrip();
    testCentreScale();
    testInFront();
    testNdc();
    testPose();
    testDiagonal();
    testOldVerticalExact();
    testAutoDistortion();
    testLimits();
    testGlobe();
    testOutline();
    return modeltest::finish("view_projection_test");
}
