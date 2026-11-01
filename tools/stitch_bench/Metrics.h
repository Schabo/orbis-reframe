#pragma once

// stitch_bench's measurements: picture only. Each one takes stitched
// equirects (or the seam strips sampled from them) and knows nothing about
// how they were stitched, so any stitcher -- the app's, a new one, Insta360
// Studio's export -- is measured the same way, and the reference can be
// measured against itself. No GPU; OpenCV for resampling and optical flow.
//
// Conventions are the stitch's: an equirect pixel (x, y) of W x H looks
// along lat = (0.5 - (y + 0.5) / H) pi, lon = ((x + 0.5) / W - 0.5) 2 pi,
// direction (cos lat sin lon, sin lat, -cos lat cos lon) -- the "sphere"
// frame. Images are 8-bit RGBA (alpha ignored) as the stitch hands them out.
// Angles are degrees unless a name says otherwise.

#include <opencv2/core.hpp>

#include <array>
#include <cmath>

#include <vector>

namespace bench {

struct V3 {
    double x = 0, y = 0, z = 0;
};

inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// A 3x3 rotation, row-major, acting on column vectors.
struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    V3 operator*(const V3& v) const
    {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
};

V3 PixelDir(double x, double y, int W, int H); // x, y continuous pixel coordinates
void DirPixel(const V3& d, int W, int H, double& x, double& y);

// The rotation from our sphere to a reference's, from refrot's Z, Y, X
// angles: R = Rz Ry Rx in the frame x at the image centre (longitude 0), y
// at longitude +90, z up; the reference's direction for our d is R d.
M3 RefRotation(double zDeg, double yDeg, double xDeg);

// Where the lenses meet, in the sphere frame: the seam circle is the great
// circle normal to `axis` (lens 0's way); `u`, `v` span it (azimuth phi =
// atan2(d.v, d.u)), elevation e = asin(d.axis), positive towards lens 0.
// `lensAxis` are the lenses' optical axes, for the radial profile.
struct SeamFrame {
    V3 axis{0, 0, 1}, u{1, 0, 0}, v{0, 1, 0};
    V3 lensAxis[2] = {{0, 0, 1}, {0, 0, -1}};
    double bandDeg = 5; // the band's half-width: where the lenses are blended
    V3 dir(double phiDeg, double eDeg) const;
    double elevationDeg(const V3& d) const;
    double azimuthDeg(const V3& d) const;
};

// The seam unwrapped: column c at azimuth (c + 0.5) / pxDeg, row r at
// elevation halfDeg - (r + 0.5) / pxDeg, so lens 0 is at the top.
struct StripGrid {
    double pxDeg = 20;
    double halfDeg = 30;
    int width() const;
    int height() const;
    double rowDeg(double r) const { return halfDeg - (r + 0.5) / pxDeg; }
    StripGrid scaled(double factor) const { return {pxDeg * factor, halfDeg}; }
};

// cv::remap maps (CV_32FC1 x, y) sampling an equirect of eqW x eqH for each
// pixel of an output: the strip (`toRef` turns our direction into the
// equirect's: null for our own) or a whole equirect of outW x outH.
struct Maps {
    cv::Mat x, y;
};
Maps StripMaps(const SeamFrame& f, const StripGrid& g, int eqW, int eqH, const M3* toRef);
Maps EquirectMaps(int outW, int outH, int eqW, int eqH, const M3& toRef);
// Bilinear, wrapping in longitude. Keeps the source's type.
cv::Mat Remap(const cv::Mat& src, const Maps& m);

// Luma (BT.709 weights) as CV_8UC1 of an RGBA image.
cv::Mat Grey(const cv::Mat& rgba);

// Per pixel of an outW x outH equirect: |elevation| off the seam circle.
cv::Mat ElevationMap(const SeamFrame& f, int W, int H);

// PSNR of ours against the reference (same size, RGBA), over the sphere and
// over the band; again after a gain per channel on ours (the least-squares
// one over unclipped pixels, `gain`), which takes out a colour difference so
// what's left is mostly geometry and sharpness. 99 = identical.
struct PsnrResult {
    double whole = 0, band = 0, wholeGain = 0, bandGain = 0;
    double gain[3] = {1, 1, 1}; // ours x gain ~ reference, R G B
};
PsnrResult ComparePsnr(const cv::Mat& ours, const cv::Mat& ref, const cv::Mat& absElevDeg,
                       double bandDeg);

// Where ours puts the reference's content: optical flow (DIS) from the
// reference to ours on contrast-normalised luma, over textured pixels with
// |latitude| <= 75. The angle between where a point is in the reference and
// in ours, by region (band: |e| <= bandDeg; near: to 20; body: past it);
// per lens, by off-axis angle in 10 degree bins (from 0), the radial
// difference (ours further from the axis: +) and the tangential one; and the
// small rotation that best explains the near and body regions (sphere frame;
// RotFitRefrot turns it into refrot's terms).
struct GeometryResult {
    static constexpr int kBins = 11;
    double bandMed = NAN, bandP95 = NAN, nearMed = NAN, nearP95 = NAN, bodyMed = NAN,
           bodyP95 = NAN;
    double radial[2][kBins], tangential[2][kBins];
    int count[2][kBins] = {};
    V3 rotation; // radians, about the sphere-frame axes
    GeometryResult();
};
GeometryResult MeasureGeometry(const cv::Mat& ours, const cv::Mat& ref, const SeamFrame& f);
// The list's refrot after a rotation fit: refrot - the fit, to first order.
void RotFitRefrot(const V3& rotation, const double refrot[3], double out[3]);

// The offset between ours and a reference along the seam: flow (DIS) from
// the reference's seam strip to ours (luma, StripGrid), the median over
// textured pixels per 5 degree block, in four parts: the band's lens 0 and
// lens 1 halves (0 < e <= bandDeg, -bandDeg <= e < 0) and a control strip on
// each side far from the seam (|e| 25..29.5, inside the strip's halfDeg),
// past where any stitcher reaches (the depth seam ramps 20 degrees past the
// band): there every stitcher is the plain lens, so what changes there is
// flow noise or the whole picture (a capture-time correction). Degrees; NaN
// where untextured.
struct SeamOffsets {
    enum Part { Band0, Band1, Control0, Control1, kParts };
    std::vector<std::array<cv::Vec2d, kParts>> blocks;
};
SeamOffsets MeasureSeamOffsets(const cv::Mat& refGrey, const cv::Mat& oursGrey, const StripGrid& g,
                               double bandDeg);

// "Alive": how much those offsets change from one frame to the next. A
// stitch whose geometry holds still keeps them, whatever passes through the
// band -- the scene moves through a fixed error -- and one that re-solves
// its warp every frame doesn't. The median and 90th percentile over the
// band's parts of |change|, and the median in the control strips (the
// floor). Degrees per frame.
struct AliveResult {
    double bandMed = NAN, bandP90 = NAN, controlMed = NAN;
    int parts = 0;
};
AliveResult CompareSeamOffsets(const SeamOffsets& prev, const SeamOffsets& cur);

// Mean squared luma gradient of a seam strip (StripGrid) in the band and
// beside it (bandDeg + 1.5..10): a stitcher's sharpness, as a ratio to the
// reference's on the same frame.
struct GradientEnergy {
    double band = 0, beside = 0;
};
GradientEnergy MeasureGradientEnergy(const cv::Mat& grey, const StripGrid& g, double bandDeg);

double Median(std::vector<double> v);
double Percentile(std::vector<double> v, double p); // p in 0..1; NaN for none

} // namespace bench
