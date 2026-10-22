// ViewProjection.h: the flat view's general perspective projection -- the
// rectilinear and stereographic ends, rays and plane points round trip, the
// centre's magnification, what is in front of the projection centre.
// The exit code is the failure count.

#include "support/ModelTest.h"

#include "ViewProjection.h"

#include <cmath>
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
    CHECK(p.fovY == 1.309f && p.distortion == 0.0f && p.yaw == 0.0f);
    view::Pose q = p;
    CHECK(q == p);
    q.distortion = 0.5f;
    CHECK(!(q == p));
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
    return modeltest::finish("view_projection_test");
}
