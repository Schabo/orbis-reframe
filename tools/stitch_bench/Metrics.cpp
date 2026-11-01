#include "Metrics.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace bench {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

V3 Cross(const V3& a, const V3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double AngleDeg(const V3& a, const V3& b)
{
    // atan2 of |a x b| and a.b: accurate for the hundredths of a degree
    // measured here, where acos of a dot product isn't.
    const V3 c = Cross(a, b);
    return std::atan2(std::sqrt(Dot(c, c)), Dot(a, b)) / kDeg;
}

double WrapDeg(double a)
{
    a = std::fmod(a + 180.0, 360.0);
    return (a < 0 ? a + 360.0 : a) - 180.0;
}

// Local contrast normalisation (8 px Gaussian), so exposure and colour
// differences between the images don't drive the flow.
cv::Mat Normalise(const cv::Mat& grey)
{
    cv::Mat x, m, d, s;
    grey.convertTo(x, CV_32F);
    cv::GaussianBlur(x, m, cv::Size(), 8);
    d = x - m;
    cv::GaussianBlur(d.mul(d), s, cv::Size(), 8);
    cv::sqrt(s, s);
    s += 4;
    cv::Mat out;
    cv::Mat n = d / s * 40 + 128;
    n.convertTo(out, CV_8U); // saturating
    return out;
}

// Where an image has texture to measure on: |Laplacian| blurred.
cv::Mat Texture(const cv::Mat& grey)
{
    cv::Mat x, l, t;
    grey.convertTo(x, CV_32F);
    cv::Laplacian(x, l, CV_32F);
    cv::GaussianBlur(cv::abs(l), t, cv::Size(), 4);
    return t;
}

// The p-th percentile of `m` (CV_32F) over every `step`-th pixel.
float MatPercentile(const cv::Mat& m, double p, int step = 4)
{
    std::vector<float> v;
    v.reserve(size_t(m.rows / step + 1) * (m.cols / step + 1));
    for (int y = 0; y < m.rows; y += step)
        for (int x = 0; x < m.cols; x += step)
            v.push_back(m.at<float>(y, x));
    if (v.empty())
        return 0;
    const size_t k = std::min(v.size() - 1, size_t(p * double(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Flow (DIS, medium) from a to b, CV_8U both: a(x) ~ b(x + flow). Wrapped
// `pad` columns either side first, for pictures that close on themselves
// (an equirect in longitude, a strip in azimuth).
cv::Mat Flow(const cv::Mat& a, const cv::Mat& b, int pad)
{
    cv::Mat pa, pb, flow;
    cv::copyMakeBorder(a, pa, 0, 0, pad, pad, cv::BORDER_WRAP);
    cv::copyMakeBorder(b, pb, 0, 0, pad, pad, cv::BORDER_WRAP);
    cv::Ptr<cv::DISOpticalFlow> dis = cv::DISOpticalFlow::create(cv::DISOpticalFlow::PRESET_MEDIUM);
    dis->calc(pa, pb, flow);
    return flow(cv::Rect(pad, 0, a.cols, a.rows)).clone();
}

} // namespace

double Percentile(std::vector<double> v, double p)
{
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !std::isfinite(x); }), v.end());
    if (v.empty())
        return NAN;
    const double idx = p * double(v.size() - 1);
    const size_t i = size_t(idx);
    std::nth_element(v.begin(), v.begin() + i, v.end());
    const double lo = v[i];
    if (i + 1 >= v.size())
        return lo;
    const double hi = *std::min_element(v.begin() + i + 1, v.end());
    return lo + (idx - double(i)) * (hi - lo);
}

double Median(std::vector<double> v) { return Percentile(std::move(v), 0.5); }

V3 PixelDir(double x, double y, int W, int H)
{
    const double lat = (0.5 - (y + 0.5) / H) * kPi;
    const double lon = ((x + 0.5) / W - 0.5) * 2.0 * kPi;
    return {std::cos(lat) * std::sin(lon), std::sin(lat), -std::cos(lat) * std::cos(lon)};
}

void DirPixel(const V3& d, int W, int H, double& x, double& y)
{
    const double lon = std::atan2(d.x, -d.z);
    const double lat = std::asin(std::clamp(d.y, -1.0, 1.0));
    x = (lon / (2 * kPi) + 0.5) * W - 0.5;
    y = (0.5 - lat / kPi) * H - 0.5;
}

M3 RefRotation(double zDeg, double yDeg, double xDeg)
{
    const double a = zDeg * kDeg, b = yDeg * kDeg, c = xDeg * kDeg;
    const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b),
                 cc = std::cos(c), sc = std::sin(c);
    // In refrot's frame P = (-s.z, s.x, s.y): x forward, y at longitude +90, z up.
    const double R[3][3] = {{ca * cb, ca * sb * sc - sa * cc, ca * sb * cc + sa * sc},
                            {sa * cb, sa * sb * sc + ca * cc, sa * sb * cc - ca * sc},
                            {-sb, cb * sc, cb * cc}};
    // s = M^T P with P = M s; the sphere-frame rotation is M^T R M.
    const double M[3][3] = {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}};
    M3 out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k)
                for (int l = 0; l < 3; ++l)
                    s += M[k][i] * R[k][l] * M[l][j];
            out.m[i][j] = s;
        }
    return out;
}

void RotFitRefrot(const V3& w, const double refrot[3], double out[3])
{
    // The fit says ours = (I + [w]x) aligned; the reference sampled at
    // R (I - [w]x) d lines up. To first order that's refrot less w in
    // refrot's frame, z y x. P = (-s.z, s.x, s.y) is a mirror of the sphere
    // frame (determinant -1), so the rotation vector also changes sign.
    const V3 p{w.z, -w.x, -w.y};
    out[0] = refrot[0] - p.z / kDeg;
    out[1] = refrot[1] - p.y / kDeg;
    out[2] = refrot[2] - p.x / kDeg;
}

V3 SeamFrame::dir(double phiDeg, double eDeg) const
{
    const double ce = std::cos(eDeg * kDeg), se = std::sin(eDeg * kDeg);
    const double cp = std::cos(phiDeg * kDeg), sp = std::sin(phiDeg * kDeg);
    return {ce * (cp * u.x + sp * v.x) + se * axis.x, ce * (cp * u.y + sp * v.y) + se * axis.y,
            ce * (cp * u.z + sp * v.z) + se * axis.z};
}

double SeamFrame::elevationDeg(const V3& d) const
{
    return std::asin(std::clamp(Dot(d, axis), -1.0, 1.0)) / kDeg;
}

double SeamFrame::azimuthDeg(const V3& d) const { return std::atan2(Dot(d, v), Dot(d, u)) / kDeg; }

int StripGrid::width() const { return int(std::lround(360.0 * pxDeg)); }
int StripGrid::height() const { return int(std::lround(2.0 * halfDeg * pxDeg)); }

Maps StripMaps(const SeamFrame& f, const StripGrid& g, int eqW, int eqH, const M3* toRef)
{
    Maps m;
    const int W = g.width(), H = g.height();
    m.x.create(H, W, CV_32F);
    m.y.create(H, W, CV_32F);
    cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y)
            for (int x = 0; x < W; ++x) {
                V3 d = f.dir((x + 0.5) / g.pxDeg, g.rowDeg(y));
                if (toRef)
                    d = *toRef * d;
                double px, py;
                DirPixel(d, eqW, eqH, px, py);
                m.x.at<float>(y, x) = float(px);
                m.y.at<float>(y, x) = float(std::clamp(py, 0.0, double(eqH - 1)));
            }
    });
    return m;
}

Maps EquirectMaps(int outW, int outH, int eqW, int eqH, const M3& toRef)
{
    Maps m;
    m.x.create(outH, outW, CV_32F);
    m.y.create(outH, outW, CV_32F);
    cv::parallel_for_(cv::Range(0, outH), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y)
            for (int x = 0; x < outW; ++x) {
                double px, py;
                DirPixel(toRef * PixelDir(x, y, outW, outH), eqW, eqH, px, py);
                m.x.at<float>(y, x) = float(px);
                m.y.at<float>(y, x) = float(std::clamp(py, 0.0, double(eqH - 1)));
            }
    });
    return m;
}

cv::Mat Remap(const cv::Mat& src, const Maps& m)
{
    // BORDER_WRAP closes the longitude; the maps clamp latitude to the first
    // and last rows, where the wrapped neighbour gets no weight.
    cv::Mat out;
    cv::remap(src, out, m.x, m.y, cv::INTER_LINEAR, cv::BORDER_WRAP);
    return out;
}

cv::Mat Grey(const cv::Mat& rgba)
{
    cv::Mat g(rgba.rows, rgba.cols, CV_8U);
    cv::parallel_for_(cv::Range(0, rgba.rows), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y) {
            const uint8_t* s = rgba.ptr<uint8_t>(y);
            uint8_t* d = g.ptr<uint8_t>(y);
            for (int x = 0; x < rgba.cols; ++x, s += 4)
                d[x] = uint8_t(std::lround(0.2126 * s[0] + 0.7152 * s[1] + 0.0722 * s[2]));
        }
    });
    return g;
}

cv::Mat ElevationMap(const SeamFrame& f, int W, int H)
{
    cv::Mat e(H, W, CV_32F);
    cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y)
            for (int x = 0; x < W; ++x)
                e.at<float>(y, x) = float(std::fabs(f.elevationDeg(PixelDir(x, y, W, H))));
    });
    return e;
}

PsnrResult ComparePsnr(const cv::Mat& ours, const cv::Mat& ref, const cv::Mat& absElevDeg,
                       double bandDeg)
{
    CV_Assert(ours.size() == ref.size() && ours.type() == CV_8UC4 && ref.type() == CV_8UC4);
    const int H = ours.rows, W = ours.cols;
    // Pass 1: the gain, over pixels unclipped in both.
    std::vector<std::array<double, 6>> acc(H);
    cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y) {
            std::array<double, 6> a{};
            const uint8_t *o = ours.ptr<uint8_t>(y), *f = ref.ptr<uint8_t>(y);
            for (int x = 0; x < W; ++x, o += 4, f += 4)
                for (int c = 0; c < 3; ++c)
                    if (o[c] > 3 && o[c] < 252 && f[c] > 3 && f[c] < 252) {
                        a[c] += double(o[c]) * f[c];
                        a[3 + c] += double(o[c]) * o[c];
                    }
            acc[y] = a;
        }
    });
    PsnrResult res;
    for (int c = 0; c < 3; ++c) {
        double of = 0, oo = 0;
        for (const auto& a : acc) {
            of += a[c];
            oo += a[3 + c];
        }
        res.gain[c] = oo > 0 ? of / oo : 1.0;
    }
    // Pass 2: squared errors, raw and with the gain, whole and band.
    std::vector<std::array<double, 5>> se(H);
    cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y) {
            std::array<double, 5> a{};
            const uint8_t *o = ours.ptr<uint8_t>(y), *f = ref.ptr<uint8_t>(y);
            const float* e = absElevDeg.ptr<float>(y);
            for (int x = 0; x < W; ++x, o += 4, f += 4) {
                double raw = 0, gained = 0;
                for (int c = 0; c < 3; ++c) {
                    const double d = double(o[c]) - f[c];
                    const double g = std::clamp(o[c] * res.gain[c], 0.0, 255.0) - f[c];
                    raw += d * d;
                    gained += g * g;
                }
                a[0] += raw;
                a[1] += gained;
                if (e[x] <= bandDeg) {
                    a[2] += raw;
                    a[3] += gained;
                    a[4] += 3;
                }
            }
            se[y] = a;
        }
    });
    std::array<double, 5> t{};
    for (const auto& a : se)
        for (int k = 0; k < 5; ++k)
            t[k] += a[k];
    auto psnr = [](double sse, double n) {
        if (n <= 0)
            return double(NAN);
        const double mse = sse / n;
        return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    };
    const double n = 3.0 * W * H;
    res.whole = psnr(t[0], n);
    res.wholeGain = psnr(t[1], n);
    res.band = psnr(t[2], t[4]);
    res.bandGain = psnr(t[3], t[4]);
    return res;
}

GeometryResult::GeometryResult()
{
    for (int l = 0; l < 2; ++l)
        for (int b = 0; b < kBins; ++b)
            radial[l][b] = tangential[l][b] = NAN;
}

GeometryResult MeasureGeometry(const cv::Mat& ours, const cv::Mat& ref, const SeamFrame& f)
{
    const int W = ours.cols, H = ours.rows;
    const cv::Mat g0 = Grey(ref), g1 = Grey(ours);
    const cv::Mat flow = Flow(Normalise(g0), Normalise(g1), W / 36);
    const cv::Mat tex = Texture(g0);
    const int y0 = int(H * (15.0 / 180.0)), y1 = int(H * (165.0 / 180.0)); // |lat| <= 75
    const float thr = MatPercentile(tex(cv::Range(y0, y1), cv::Range::all()), 0.4);

    std::vector<double> band, nearr, body;
    std::vector<double> rad[2][GeometryResult::kBins], tan[2][GeometryResult::kBins];
    struct Sample {
        V3 d, delta;
    };
    std::vector<Sample> fit;
    for (int y = y0; y < y1; y += 2) {
        const uint8_t* lum = g0.ptr<uint8_t>(y);
        for (int x = 0; x < W; x += 2) {
            if (tex.at<float>(y, x) <= thr || lum[x] < 6 || lum[x] > 249)
                continue;
            const cv::Vec2f fl = flow.at<cv::Vec2f>(y, x);
            const V3 d0 = PixelDir(x, y, W, H), d1 = PixelDir(x + fl[0], y + fl[1], W, H);
            const double ang = AngleDeg(d0, d1);
            const double e = f.elevationDeg(d0), ae = std::fabs(e);
            (ae <= f.bandDeg ? band : ae <= 20 ? nearr : body).push_back(ang);
            const int lens = e >= 0 ? 0 : 1;
            const double th0 = AngleDeg(d0, f.lensAxis[lens]), th1 = AngleDeg(d1, f.lensAxis[lens]);
            const int bin = int(th0 / 10.0);
            if (bin < GeometryResult::kBins) {
                rad[lens][bin].push_back(th1 - th0);
                const double dpsi = WrapDeg(f.azimuthDeg(d1) - f.azimuthDeg(d0));
                tan[lens][bin].push_back((lens == 0 ? dpsi : -dpsi) * std::sin(th0 * kDeg));
            }
            if (ae > f.bandDeg && (x % 4 == 0) && (y % 4 == 0))
                fit.push_back({d0, {d1.x - d0.x, d1.y - d0.y, d1.z - d0.z}});
        }
    }
    GeometryResult r;
    r.bandMed = Median(band);
    r.bandP95 = Percentile(band, 0.95);
    r.nearMed = Median(nearr);
    r.nearP95 = Percentile(nearr, 0.95);
    r.bodyMed = Median(body);
    r.bodyP95 = Percentile(body, 0.95);
    for (int l = 0; l < 2; ++l)
        for (int b = 0; b < GeometryResult::kBins; ++b) {
            r.count[l][b] = int(rad[l][b].size());
            if (rad[l][b].size() >= 200) {
                r.radial[l][b] = Median(rad[l][b]);
                r.tangential[l][b] = Median(tan[l][b]);
            }
        }

    // The rotation: least squares on delta = w x d (A w with A = -[d]x),
    // then again without the samples more than 3x the median residual off
    // (near things, flow errors).
    auto solve = [&](const std::vector<char>* keep) {
        cv::Matx33d AtA = cv::Matx33d::zeros();
        cv::Vec3d Atb(0, 0, 0);
        for (size_t i = 0; i < fit.size(); ++i) {
            if (keep && !(*keep)[i])
                continue;
            const V3& d = fit[i].d;
            const cv::Matx33d A(0, d.z, -d.y, -d.z, 0, d.x, d.y, -d.x, 0);
            AtA += A.t() * A;
            Atb += A.t() * cv::Vec3d(fit[i].delta.x, fit[i].delta.y, fit[i].delta.z);
        }
        cv::Vec3d w(0, 0, 0);
        cv::solve(AtA, Atb, w, cv::DECOMP_SVD);
        return w;
    };
    if (fit.size() >= 100) {
        cv::Vec3d w = solve(nullptr);
        std::vector<double> res(fit.size());
        for (size_t i = 0; i < fit.size(); ++i) {
            const V3 p = Cross({w[0], w[1], w[2]}, fit[i].d);
            const V3 e{fit[i].delta.x - p.x, fit[i].delta.y - p.y, fit[i].delta.z - p.z};
            res[i] = std::sqrt(Dot(e, e));
        }
        const double cut = 3.0 * Median(res);
        std::vector<char> keep(fit.size());
        for (size_t i = 0; i < fit.size(); ++i)
            keep[i] = res[i] <= cut;
        w = solve(&keep);
        r.rotation = {w[0], w[1], w[2]};
    }
    return r;
}

SeamOffsets MeasureSeamOffsets(const cv::Mat& refGrey, const cv::Mat& oursGrey, const StripGrid& g,
                               double bandDeg)
{
    const int W = oursGrey.cols, H = oursGrey.rows;
    const int bw = std::max(1, int(std::lround(5.0 * g.pxDeg)));
    const cv::Mat flow = Flow(Normalise(refGrey), Normalise(oursGrey), bw);
    const cv::Mat tex = Texture(refGrey);
    const float thr = MatPercentile(tex, 0.4);
    const double ctlLo = 25.0, ctlHi = std::min(29.5, g.halfDeg - 0.5);
    auto partOf = [&](double e) {
        const double ae = std::fabs(e);
        if (ae <= bandDeg)
            return e > 0 ? SeamOffsets::Band0 : SeamOffsets::Band1;
        if (ae >= ctlLo && ae <= ctlHi)
            return e > 0 ? SeamOffsets::Control0 : SeamOffsets::Control1;
        return SeamOffsets::kParts;
    };
    SeamOffsets out;
    for (int x0 = 0; x0 + bw <= W; x0 += bw) {
        std::vector<double> vx[SeamOffsets::kParts], vy[SeamOffsets::kParts];
        for (int y = 0; y < H; ++y) {
            const int part = partOf(g.rowDeg(y));
            if (part == SeamOffsets::kParts)
                continue;
            for (int x = x0; x < x0 + bw; ++x)
                if (tex.at<float>(y, x) > thr) {
                    const cv::Vec2f f = flow.at<cv::Vec2f>(y, x);
                    vx[part].push_back(f[0]);
                    vy[part].push_back(f[1]);
                }
        }
        std::array<cv::Vec2d, SeamOffsets::kParts> b;
        for (int p = 0; p < SeamOffsets::kParts; ++p)
            b[p] = vx[p].size() >= 30 ? cv::Vec2d(Median(vx[p]) / g.pxDeg, Median(vy[p]) / g.pxDeg)
                                      : cv::Vec2d(NAN, NAN);
        out.blocks.push_back(b);
    }
    return out;
}

AliveResult CompareSeamOffsets(const SeamOffsets& prev, const SeamOffsets& cur)
{
    std::vector<double> band, ctl;
    const size_t n = std::min(prev.blocks.size(), cur.blocks.size());
    for (size_t i = 0; i < n; ++i)
        for (int p = 0; p < SeamOffsets::kParts; ++p) {
            const cv::Vec2d a = prev.blocks[i][p], b = cur.blocks[i][p];
            if (!std::isfinite(a[0]) || !std::isfinite(b[0]))
                continue;
            const double d = std::hypot(b[0] - a[0], b[1] - a[1]);
            (p == SeamOffsets::Band0 || p == SeamOffsets::Band1 ? band : ctl).push_back(d);
        }
    AliveResult r;
    r.parts = int(band.size());
    r.bandMed = Median(band);
    r.bandP90 = Percentile(band, 0.9);
    r.controlMed = Median(ctl);
    return r;
}

GradientEnergy MeasureGradientEnergy(const cv::Mat& grey, const StripGrid& g, double bandDeg)
{
    cv::Mat x, gx, gy;
    grey.convertTo(x, CV_32F);
    cv::Sobel(x, gx, CV_32F, 1, 0);
    cv::Sobel(x, gy, CV_32F, 0, 1);
    const cv::Mat e2 = gx.mul(gx) + gy.mul(gy);
    double sb = 0, ss = 0;
    long nb = 0, ns = 0;
    for (int y = 1; y + 1 < e2.rows; ++y) {
        const double ae = std::fabs(g.rowDeg(y));
        const bool band = ae <= bandDeg, beside = ae >= bandDeg + 1.5 && ae <= bandDeg + 10;
        if (!band && !beside)
            continue;
        const double row = cv::sum(e2.row(y))[0];
        (band ? sb : ss) += row;
        (band ? nb : ns) += e2.cols;
    }
    return {nb ? sb / double(nb) : NAN, ns ? ss / double(ns) : NAN};
}

} // namespace bench
