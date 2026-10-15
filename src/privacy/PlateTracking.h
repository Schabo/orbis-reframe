#pragma once

// The plate scanner's geometry and its quad tracker, private to
// src/privacy (PlateScanner.cpp, PlateTracking.cpp): directions on the
// levelled sphere, pinhole views of it, detections as boxes on it, and
// following a plate's quad from frame to frame by optical flow, checked
// against the plate as it was last seen.

#include <opencv2/core.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace privacy::plates {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

// --- sphere geometry (levelled frame) ------------------------------------------
// Direction from longitude / latitude, the stitcher's equirect convention:
// lon 0 = -z (forward), +lon to the right (+x), +lat up (+y).
cv::Vec3d dirOf(double lon, double lat);

void lonLatOf(const cv::Vec3d& d, double& lon, double& lat);

// Local horizon basis at a direction: right along +lon, up toward +lat.
void basisAt(double lon, double lat, cv::Vec3d& fwd, cv::Vec3d& right, cv::Vec3d& up);

// A pinhole view in the levelled frame.
struct View {
    cv::Vec3d fwd, right, up;
    double f = 1, cx = 0, cy = 0;
    int w = 0, h = 0;
    double lonDeg = 0, latDeg = 0, hfovDeg = 0; // as made
    static View make(double lonDeg, double latDeg, double hfovDeg, int w, int h)
    {
        View v;
        v.lonDeg = lonDeg;
        v.latDeg = latDeg;
        v.hfovDeg = hfovDeg;
        basisAt(lonDeg * kDegToRad, latDeg * kDegToRad, v.fwd, v.right, v.up);
        v.w = w;
        v.h = h;
        v.f = 0.5 * w / std::tan(0.5 * hfovDeg * kDegToRad);
        v.cx = 0.5 * w;
        v.cy = 0.5 * h;
        return v;
    }
    cv::Vec3d ray(double px, double py) const
    {
        return cv::normalize(fwd * f + right * (px - cx) - up * (py - cy));
    }
    bool project(const cv::Vec3d& d, cv::Point2f& p) const
    {
        const double z = d.dot(fwd);
        if (z <= 1e-3)
            return false;
        p.x = static_cast<float>(cx + f * d.dot(right) / z);
        p.y = static_cast<float>(cy - f * d.dot(up) / z);
        return true;
    }
};

// A detection on the sphere: centre + angular extent along the local horizon
// axes, levelled frame.
struct SphereBox {
    double lon = 0, lat = 0, w = 0, h = 0;
    float score = 0;
    bool cut = false; // touches its tile's edge: maybe only part of the plate
};

double angleBetween(const SphereBox& a, const SphereBox& b);

using Quad = std::array<cv::Vec3d, 4>; // corner directions TL, TR, BR, BL

// The box as a gnomonic rectangle around its centre, grown by dilW x dilH.
Quad quadFromBox(const SphereBox& b, double dilW = 1.0, double dilH = 1.0);

cv::Vec3d centroidDir(const Quad& q);

// Grown about its centre by s, in the tangent plane there.
Quad growQuad(const Quad& q, double s);

double angleOf(const cv::Vec3d& a, const cv::Vec3d& b);

// The longer diagonal, radians.
double quadSize(const Quad& q);

SphereBox lerpBox(const SphereBox& a, const SphereBox& b, double t);

// --- quad tracker -----------------------------------------------------------------
constexpr int kQuadPx = 384; // tracking view, square

// Plate ~1/3 of the view across: enough context for the pyramid, enough
// zoom that a ~1 deg plate isn't a handful of pixels.
View viewForQuad(const Quad& q);

// Quad corners in the view's image (OpenCV pixel-centre coordinates).
bool projectQuad(const View& v, const Quad& q, std::vector<cv::Point2f>& px);

bool insideImage(const std::vector<cv::Point2f>& px, int w, int h);

cv::Mat rectify(const cv::Mat& gray, const std::vector<cv::Point2f>& quadPx, cv::Size sz);

// What view `a` saw, as view `b` would see it: both share the centre of
// projection, so it's an exact homography (fixed by b's four corners).
bool warpView(const cv::Mat& img, const View& a, const View& b, cv::Mat& out);

// Normalised cross-correlation of the inner 80 % (the detector's box carries
// some background at its edges, which changes as the vehicle moves).
double nccInner(const cv::Mat& a, const cv::Mat& b);

struct QuadTracker {
    int track = -1;
    Quad q;
    View view;    // the view the quad was last placed in ...
    cv::Mat gray; // ... and that frame through it
    cv::Mat tmpl; // the plate face-on, at the last detection
    cv::Size tmplSize;
    int sinceAnchor = 0, unverified = 0;
    int holdLeft = -1; // >= 0: lost, held this many more frames
    // This frame's provisional step (stepQuad), committed unless a
    // detection re-anchors the quad.
    bool stepOk = false;
    Quad stepQ;
    double stepNcc = 1.0;
};

// Anchor on quad `q` (its height / width `hOverW`), in view `v` of the frame
// (`gray`). False when the quad doesn't sit inside the view.
bool anchorQuad(QuadTracker& t, const Quad& q, double hOverW, const View& v, const cv::Mat& gray);

// ... on a detection.
bool anchorQuad(QuadTracker& t, const SphereBox& b, const View& v, const cv::Mat& gray);

// A quad's centre and mean angular width / height, as a box.
SphereBox boxOfQuad(const Quad& q);

// The quad turned by the rotation that takes direction a to b.
// `q` turned by the rotation taking `a` to `b`, its angle scaled by `scale`
// and then capped at `maxAngle` (radians).
Quad rotateQuad(const Quad& q, const cv::Vec3d& a, const cv::Vec3d& b, double scale = 1.0,
                double maxAngle = kPi);

// One frame forward, provisionally: the last frame (warped into this frame's
// view `v`) against this one (`cur`, through `v`). Sets stepOk / stepQ /
// stepNcc.
void stepQuad(QuadTracker& t, const View& v, const cv::Mat& cur);

} // namespace privacy::plates
