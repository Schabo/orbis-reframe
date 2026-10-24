// ViewProjection.h: the flat view's general perspective projection -- the
// rectilinear and stereographic ends, rays and plane points round trip, the
// centre's magnification, what is in front of the projection centre; the
// diagonal field of view, the automatic distortion and the angle limits.
// The exit code is the failure count.

#include "support/ModelTest.h"

#include "ViewProjection.h"

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
    // The default frames what the old default, 75 degrees up and down, did
    // at 16:9.
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
    return modeltest::finish("view_projection_test");
}
