#include "LensFit.h"

#include "ChartBoard.h"

#include <ClipMetadata.h>
#include <FisheyeProjection.h>

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>

namespace calib {

namespace {

constexpr double kDeg = std::numbers::pi / 180;
constexpr int NI = 14; // fx fy cx cy k1..k4 p1 p2 s1..s4
constexpr int NG = 2 * NI + 3; // both lenses + the rig rotation

// One lens's model in the stitch's units (the stream's pixels) and the
// calibration's image axes (ours with x and y negated, LensProjection).
struct Intr {
    double xi = 2;
    double p[NI] = {};
    // A lens guard's radial scale on top (the stitch's accessory profile),
    // or none: only the file's model has one; the fit has the guard in it.
    const insv::FisheyeStitcher* acc = nullptr;
};

Intr FromProjection(const insv::LensProjection& L)
{
    Intr m;
    m.xi = L.xi;
    const double v[NI] = {L.fx, L.fy, L.cx, L.cy, L.k[0], L.k[1], L.k[2], L.k[3],
                          L.p[0], L.p[1], L.s[0], L.s[1], L.s[2], L.s[3]};
    std::copy(v, v + NI, m.p);
    return m;
}

// Calibration-axes ray (any length) -> pixel; false if behind the mirror.
bool Project(const Intr& m, const cv::Vec3d& X, double& u, double& v)
{
    const double n = cv::norm(X);
    const double zs = X[2] / n + m.xi;
    if (zs <= 1e-6)
        return false;
    const double x = X[0] / n / zs, y = X[1] / n / zs;
    const double* q = m.p;
    const double r2 = x * x + y * y, r4 = r2 * r2;
    const double rad = 1 + q[4] * r2 + q[5] * r4 + q[6] * r4 * r2 + q[7] * r4 * r4;
    const double xd = x * rad + 2 * q[8] * x * y + q[9] * (r2 + 2 * x * x) + q[10] * r2 + q[11] * r4;
    const double yd = y * rad + q[8] * (r2 + 2 * y * y) + 2 * q[9] * x * y + q[12] * r2 + q[13] * r4;
    const double a = m.acc ? m.acc->accessoryScale(std::acos(std::clamp(X[2] / n, -1.0, 1.0))) : 1.0;
    u = q[2] + q[0] * xd * a;
    v = q[3] + q[1] * yd * a;
    return true;
}

bool UnprojectBare(const Intr& m, double u, double v, cv::Vec3d& ray);

// Pixel -> unit ray: Newton on the distortion, then MEI's closed inverse;
// a guard's scale (which depends on the angle) by fixed-point iteration.
bool Unproject(const Intr& m, double u, double v, cv::Vec3d& ray)
{
    if (!UnprojectBare(m, u, v, ray))
        return false;
    if (!m.acc)
        return true;
    for (int it = 0; it < 6; ++it) {
        const double a = m.acc->accessoryScale(std::acos(std::clamp(ray[2], -1.0, 1.0)));
        if (!UnprojectBare(m, m.p[2] + (u - m.p[2]) / a, m.p[3] + (v - m.p[3]) / a, ray))
            return false;
    }
    return true;
}

bool UnprojectBare(const Intr& m, double u, double v, cv::Vec3d& ray)
{
    const double xd = (u - m.p[2]) / m.p[0], yd = (v - m.p[3]) / m.p[1];
    const double* q = m.p;
    auto f = [q](double a, double b, double& fx, double& fy) {
        const double r2 = a * a + b * b, r4 = r2 * r2;
        const double rad = 1 + q[4] * r2 + q[5] * r4 + q[6] * r4 * r2 + q[7] * r4 * r4;
        fx = a * rad + 2 * q[8] * a * b + q[9] * (r2 + 2 * a * a) + q[10] * r2 + q[11] * r4;
        fy = b * rad + q[8] * (r2 + 2 * b * b) + 2 * q[9] * a * b + q[12] * r2 + q[13] * r4;
    };
    double x = xd, y = yd;
    for (int it = 0; it < 50; ++it) {
        double fx, fy, ax, ay, bx, by;
        const double h = 1e-7;
        f(x, y, fx, fy);
        f(x + h, y, ax, ay);
        f(x, y + h, bx, by);
        const double j00 = (ax - fx) / h, j10 = (ay - fy) / h, j01 = (bx - fx) / h,
                     j11 = (by - fy) / h;
        const double ex = fx - xd, ey = fy - yd, det = j00 * j11 - j01 * j10;
        if (std::fabs(det) < 1e-15)
            return false;
        const double dx = (j11 * ex - j01 * ey) / det, dy = (-j10 * ex + j00 * ey) / det;
        x -= dx;
        y -= dy;
        if (std::fabs(dx) + std::fabs(dy) < 1e-12)
            break;
    }
    const double r2 = x * x + y * y, xi = m.xi;
    const double disc = 1 + (1 - xi * xi) * r2;
    if (disc < 0)
        return false;
    const double s = (xi + std::sqrt(disc)) / (r2 + 1);
    ray = cv::Vec3d(s * x, s * y, s - xi);
    ray /= cv::norm(ray);
    return true;
}

// Rotation vector <-> matrix (Rodrigues; written out, so this needs no
// OpenCV module beyond core).
cv::Matx33d Rot(const cv::Vec3d& r)
{
    const double th = cv::norm(r);
    if (th < 1e-15)
        return cv::Matx33d::eye();
    const cv::Vec3d k = r / th;
    const double c = std::cos(th), s = std::sin(th);
    const cv::Matx33d K(0, -k[2], k[1], k[2], 0, -k[0], -k[1], k[0], 0);
    return cv::Matx33d::eye() * c + K * s + (k * k.t()) * (1 - c);
}

cv::Vec3d RotVec(const cv::Matx33d& R)
{
    // The angle from both its cosine and sine (atan2), which stays exact
    // near 180 degrees, where lens 0 -> lens 1 is.
    const cv::Vec3d a(R(2, 1) - R(1, 2), R(0, 2) - R(2, 0), R(1, 0) - R(0, 1));
    const double sn = 0.5 * cv::norm(a), c = 0.5 * (R(0, 0) + R(1, 1) + R(2, 2) - 1);
    const double th = std::atan2(sn, c);
    if (th < 1e-9)
        return a * 0.5;
    if (sn > 1e-6)
        return a * (th / (2 * sn));
    // Within ~1e-6 rad of 180 degrees: the axis from the symmetric part.
    const cv::Matx33d B = (R + R.t()) * 0.5 - cv::Matx33d::eye() * c;
    int col = 0;
    for (int i = 1; i < 3; ++i)
        if (B(i, i) > B(col, col))
            col = i;
    cv::Vec3d k(B(0, col), B(1, col), B(2, col));
    k /= cv::norm(k);
    return k * th;
}

struct Obs {
    int id;
    double u, v;
};
struct Frame {
    int frame = 0;
    std::vector<Obs> obs;
    double motion = 1e9; // px per video frame, from the neighbouring samples
    cv::Vec3d r, t;      // pose: Xc = R(r) Pb + t (mm)
    bool ok = false;
};

double Huber(double e, double d) { return e <= d ? 1.0 : d / e; }

class Fitter {
public:
    explicit Fitter(double squareMm) : sq_(squareMm) {}

    cv::Vec3d boardPoint(int id) const
    {
        double x, y;
        ChartCornerPosition(id, x, y);
        return {x * sq_, y * sq_, 0};
    }

    // The detections of one lens as frames, with each frame's motion: the
    // median displacement of shared corners to the nearest sample either
    // side, per video frame.
    std::vector<Frame> frames(const std::vector<ChartView>& views) const
    {
        std::vector<Frame> out;
        for (const ChartView& v : views) {
            Frame f;
            f.frame = v.frame;
            for (const ChartCorner& c : v.corners)
                if (c.id >= 0 && c.id < kChartCorners)
                    f.obs.push_back({c.id, c.u, c.v});
            out.push_back(std::move(f));
        }
        for (size_t i = 0; i < out.size(); ++i) {
            double best = 1e9;
            for (int d : {-1, 1}) {
                const long j = long(i) + d;
                if (j < 0 || j >= long(out.size()))
                    continue;
                const int gap = std::abs(out[j].frame - out[i].frame);
                if (gap == 0 || gap > 6)
                    continue;
                std::map<int, std::pair<double, double>> m;
                for (const Obs& o : out[j].obs)
                    m[o.id] = {o.u, o.v};
                std::vector<double> ds;
                for (const Obs& o : out[i].obs)
                    if (auto it = m.find(o.id); it != m.end())
                        ds.push_back(std::hypot(o.u - it->second.first, o.v - it->second.second));
                if (ds.size() < 6)
                    continue;
                std::nth_element(ds.begin(), ds.begin() + ds.size() / 2, ds.end());
                best = std::min(best, ds[ds.size() / 2] / gap);
            }
            out[i].motion = best;
        }
        return out;
    }

    void residuals(const Intr& m, const cv::Vec3d& r, const cv::Vec3d& t, const Frame& f,
                   const std::vector<double>& w, std::vector<double>& res) const
    {
        const cv::Matx33d R = Rot(r);
        res.resize(2 * f.obs.size());
        for (size_t k = 0; k < f.obs.size(); ++k) {
            double u, v;
            if (!Project(m, R * boardPoint(f.obs[k].id) + t, u, v))
                u = v = 1e4;
            const double s = std::sqrt(w[k]);
            res[2 * k] = s * (u - f.obs[k].u);
            res[2 * k + 1] = s * (v - f.obs[k].v);
        }
    }

    // A first pose from the rays: turned so their mean is +z, the board's
    // plane-to-image homography (DLT), decomposed.
    bool initPose(const Intr& m, Frame& f) const
    {
        std::vector<cv::Vec3d> rays;
        cv::Vec3d mean(0, 0, 0);
        for (const Obs& o : f.obs) {
            cv::Vec3d d;
            if (!Unproject(m, o.u, o.v, d))
                return false;
            rays.push_back(d);
            mean += d;
        }
        mean /= cv::norm(mean);
        cv::Vec3d axis = mean.cross(cv::Vec3d(0, 0, 1));
        const double s = cv::norm(axis), c = mean[2];
        cv::Matx33d Q = cv::Matx33d::eye();
        if (s > 1e-9)
            Q = Rot(axis / s * std::atan2(s, c));
        std::vector<cv::Vec2d> img, obj;
        for (size_t k = 0; k < rays.size(); ++k) {
            const cv::Vec3d q = Q * rays[k];
            if (q[2] < 0.2)
                continue;
            img.push_back({q[0] / q[2], q[1] / q[2]});
            const cv::Vec3d P = boardPoint(f.obs[k].id);
            obj.push_back({P[0] / sq_, P[1] / sq_}); // board units, well scaled
        }
        if (img.size() < 6)
            return false;
        // DLT: img ~ H obj.
        cv::Mat A(int(2 * img.size()), 9, CV_64F);
        for (size_t k = 0; k < img.size(); ++k) {
            const double X = obj[k][0], Y = obj[k][1], x = img[k][0], y = img[k][1];
            const double r0[9] = {X, Y, 1, 0, 0, 0, -x * X, -x * Y, -x};
            const double r1[9] = {0, 0, 0, X, Y, 1, -y * X, -y * Y, -y};
            std::copy(r0, r0 + 9, A.ptr<double>(int(2 * k)));
            std::copy(r1, r1 + 9, A.ptr<double>(int(2 * k + 1)));
        }
        cv::Mat w, u, vt;
        cv::SVD::compute(A, w, u, vt, cv::SVD::FULL_UV);
        const cv::Mat h = vt.row(8);
        cv::Matx33d H;
        for (int i = 0; i < 9; ++i)
            H(i / 3, i % 3) = h.at<double>(i);
        cv::Vec3d h1(H(0, 0), H(1, 0), H(2, 0)), h2(H(0, 1), H(1, 1), H(2, 1)),
            h3(H(0, 2), H(1, 2), H(2, 2));
        double lam = 1.0 / cv::norm(h1);
        if (h3[2] * lam < 0)
            lam = -lam; // the board in front
        const cv::Vec3d r1 = h1 * lam, r2 = h2 * lam, r3 = r1.cross(r2);
        cv::Matx33d R(r1[0], r2[0], r3[0], r1[1], r2[1], r3[1], r1[2], r2[2], r3[2]);
        cv::Mat sw, su, svt;
        cv::SVD::compute(cv::Mat(R), sw, su, svt);
        R = cv::Matx33d(cv::Mat(su * svt));
        if (cv::determinant(R) < 0)
            return false;
        // In squares Xc = R b + t, so in mm (P = sq b) Xc = R P + sq t.
        const cv::Vec3d t = h3 * lam * sq_;
        f.r = RotVec(Q.t() * R);
        f.t = Q.t() * t;
        return true;
    }

    // Bundle adjustment of one lens: poses, and the intrinsics unless
    // `fixIntr`. Returns the RMS over corners within `drop`.
    double adjust(Intr& m, std::vector<Frame>& frames, bool fixIntr, int iters, double huber,
                  double drop) const
    {
        const int nf = int(frames.size());
        std::vector<std::vector<double>> W(nf);
        for (int i = 0; i < nf; ++i)
            W[i].assign(frames[i].obs.size(), 1.0);
        auto cost = [&](const Intr& mm, const std::vector<Frame>& fr) {
            std::vector<double> c(nf, 0.0);
            cv::parallel_for_(cv::Range(0, nf), [&](const cv::Range& rg) {
                std::vector<double> res;
                for (int i = rg.start; i < rg.end; ++i) {
                    if (!fr[i].ok)
                        continue;
                    residuals(mm, fr[i].r, fr[i].t, fr[i], W[i], res);
                    for (double x : res)
                        c[i] += x * x;
                }
            });
            double s = 0;
            for (double x : c)
                s += x;
            return s;
        };
        double lambda = 1e-3;
        std::vector<cv::Matx<double, NI, NI>> Ui(nf);
        std::vector<cv::Matx<double, NI, 1>> bii(nf);
        std::vector<cv::Matx66d> V(nf);
        std::vector<cv::Matx<double, NI, 6>> Wm(nf);
        std::vector<cv::Matx61d> bp(nf);
        for (int it = 0; it < iters; ++it) {
            cv::parallel_for_(cv::Range(0, nf), [&](const cv::Range& rg) {
                std::vector<double> res;
                for (int i = rg.start; i < rg.end; ++i) {
                    if (!frames[i].ok)
                        continue;
                    std::vector<double> one(frames[i].obs.size(), 1.0);
                    residuals(m, frames[i].r, frames[i].t, frames[i], one, res);
                    for (size_t k = 0; k < frames[i].obs.size(); ++k) {
                        const double e = std::hypot(res[2 * k], res[2 * k + 1]);
                        W[i][k] = e > drop ? 0.0 : Huber(e, huber);
                    }
                }
            });
            const double c0 = cost(m, frames);
            double hstep[NI];
            for (int j = 0; j < NI; ++j)
                hstep[j] = 1e-6 * std::max(1.0, std::fabs(m.p[j]));
            cv::parallel_for_(cv::Range(0, nf), [&](const cv::Range& rg) {
                for (int i = rg.start; i < rg.end; ++i) {
                    const Frame& f = frames[i];
                    Ui[i] = cv::Matx<double, NI, NI>::zeros();
                    bii[i] = cv::Matx<double, NI, 1>::zeros();
                    V[i] = cv::Matx66d::zeros();
                    Wm[i] = cv::Matx<double, NI, 6>::zeros();
                    bp[i] = cv::Matx61d::zeros();
                    if (!f.ok)
                        continue;
                    std::vector<double> r0, r1;
                    residuals(m, f.r, f.t, f, W[i], r0);
                    const int nr = int(r0.size());
                    std::vector<std::array<double, 6>> Jp(nr);
                    for (int j = 0; j < 6; ++j) {
                        cv::Vec3d r = f.r, t = f.t;
                        const double h = j < 3 ? 1e-7 : 1e-5;
                        (j < 3 ? r[j] : t[j - 3]) += h;
                        residuals(m, r, t, f, W[i], r1);
                        for (int k = 0; k < nr; ++k)
                            Jp[k][j] = (r1[k] - r0[k]) / h;
                    }
                    std::vector<std::array<double, NI>> Ji(fixIntr ? 0 : nr);
                    if (!fixIntr)
                        for (int j = 0; j < NI; ++j) {
                            Intr mm = m;
                            mm.p[j] += hstep[j];
                            residuals(mm, f.r, f.t, f, W[i], r1);
                            for (int k = 0; k < nr; ++k)
                                Ji[k][j] = (r1[k] - r0[k]) / hstep[j];
                        }
                    for (int k = 0; k < nr; ++k) {
                        for (int a = 0; a < 6; ++a) {
                            bp[i](a) += Jp[k][a] * r0[k];
                            for (int c = 0; c < 6; ++c)
                                V[i](a, c) += Jp[k][a] * Jp[k][c];
                        }
                        if (!fixIntr)
                            for (int a = 0; a < NI; ++a) {
                                bii[i](a) += Ji[k][a] * r0[k];
                                for (int c = 0; c < NI; ++c)
                                    Ui[i](a, c) += Ji[k][a] * Ji[k][c];
                                for (int c = 0; c < 6; ++c)
                                    Wm[i](a, c) += Ji[k][a] * Jp[k][c];
                            }
                    }
                }
            });
            cv::Matx<double, NI, NI> U = cv::Matx<double, NI, NI>::zeros();
            cv::Matx<double, NI, 1> bi = cv::Matx<double, NI, 1>::zeros();
            for (int i = 0; i < nf; ++i) {
                U += Ui[i];
                bi += bii[i];
            }
            bool stepped = false;
            for (int tries = 0; tries < 10 && !stepped; ++tries) {
                Intr m2 = m;
                std::vector<Frame> f2 = frames;
                std::vector<cv::Matx66d> Vinv(nf);
                for (int i = 0; i < nf; ++i) {
                    if (!frames[i].ok)
                        continue;
                    cv::Matx66d v = V[i];
                    for (int a = 0; a < 6; ++a)
                        v(a, a) *= 1 + lambda;
                    Vinv[i] = v.inv(cv::DECOMP_CHOLESKY);
                }
                cv::Matx<double, NI, 1> di = cv::Matx<double, NI, 1>::zeros();
                if (!fixIntr) {
                    cv::Matx<double, NI, NI> S = U;
                    for (int a = 0; a < NI; ++a)
                        S(a, a) *= 1 + lambda;
                    cv::Matx<double, NI, 1> rhs = -bi;
                    for (int i = 0; i < nf; ++i) {
                        if (!frames[i].ok)
                            continue;
                        const cv::Matx<double, NI, 6> WV = Wm[i] * Vinv[i];
                        S -= WV * Wm[i].t();
                        rhs += WV * bp[i];
                    }
                    cv::Mat sol;
                    cv::solve(cv::Mat(S), cv::Mat(rhs), sol, cv::DECOMP_SVD);
                    for (int j = 0; j < NI; ++j) {
                        di(j) = sol.at<double>(j);
                        m2.p[j] += di(j);
                    }
                }
                for (int i = 0; i < nf; ++i) {
                    if (!frames[i].ok)
                        continue;
                    const cv::Matx61d dp = Vinv[i] * (-bp[i] - Wm[i].t() * di);
                    for (int a = 0; a < 3; ++a) {
                        f2[i].r[a] += dp(a);
                        f2[i].t[a] += dp(a + 3);
                    }
                }
                if (cost(m2, f2) < c0) {
                    m = m2;
                    frames = std::move(f2);
                    lambda = std::max(1e-7, lambda / 4);
                    stepped = true;
                } else {
                    lambda *= 8;
                }
            }
            if (!stepped)
                break; // converged
        }
        double ss = 0;
        int n = 0;
        std::vector<double> res;
        for (const Frame& f : frames) {
            if (!f.ok)
                continue;
            std::vector<double> one(f.obs.size(), 1.0);
            residuals(m, f.r, f.t, f, one, res);
            for (size_t k = 0; k < f.obs.size(); ++k) {
                const double e2 = res[2 * k] * res[2 * k] + res[2 * k + 1] * res[2 * k + 1];
                if (std::sqrt(e2) > drop)
                    continue;
                ss += e2;
                ++n;
            }
        }
        return std::sqrt(ss / std::max(1, n));
    }

private:
    double sq_;
};

// --- both lenses and the rig --------------------------------------------

struct RigFrame {
    int frame = 0;
    const Frame* f[2] = {nullptr, nullptr};
    cv::Vec3d r, t; // board pose in lens 0's calibration frame
};
struct Rig {
    Intr m[2];
    cv::Vec3d r, t; // X_c1 = R(r) X_c0 + t (mm); t held
};

double& GParam(Rig& g, int j)
{
    if (j < NI)
        return g.m[0].p[j];
    if (j < 2 * NI)
        return g.m[1].p[j - NI];
    return g.r[j - 2 * NI];
}

class RigFitter {
public:
    explicit RigFitter(const Fitter& f) : fit_(f) {}

    void residuals(const Rig& g, const RigFrame& rf, const cv::Vec3d& r, const cv::Vec3d& t,
                   const std::array<std::vector<double>, 2>& w, std::vector<double>& res) const
    {
        res.clear();
        const cv::Matx33d R = Rot(r), Rr = Rot(g.r);
        for (int l = 0; l < 2; ++l) {
            if (!rf.f[l])
                continue;
            for (size_t k = 0; k < rf.f[l]->obs.size(); ++k) {
                const Obs& o = rf.f[l]->obs[k];
                cv::Vec3d X = R * fit_.boardPoint(o.id) + t;
                if (l == 1)
                    X = Rr * X + g.t;
                double u, v;
                if (!Project(g.m[l], X, u, v))
                    u = v = 1e4;
                const double s = std::sqrt(w[l][k]);
                res.push_back(s * (u - o.u));
                res.push_back(s * (v - o.v));
            }
        }
    }

    void adjust(Rig& g, std::vector<RigFrame>& fr, int iters, double huber, double drop,
                const std::function<void(int)>& onIter) const
    {
        const int nf = int(fr.size());
        std::vector<std::array<std::vector<double>, 2>> W(nf);
        auto weights = [&] {
            cv::parallel_for_(cv::Range(0, nf), [&](const cv::Range& rg) {
                std::vector<double> res;
                for (int i = rg.start; i < rg.end; ++i) {
                    for (int l = 0; l < 2; ++l)
                        W[i][l].assign(fr[i].f[l] ? fr[i].f[l]->obs.size() : 0, 1.0);
                    const std::array<std::vector<double>, 2> one = W[i];
                    residuals(g, fr[i], fr[i].r, fr[i].t, one, res);
                    size_t q = 0;
                    for (int l = 0; l < 2; ++l)
                        for (size_t k = 0; k < W[i][l].size(); ++k, q += 2) {
                            const double e = std::hypot(res[q], res[q + 1]);
                            W[i][l][k] = e > drop ? 0.0 : Huber(e, huber);
                        }
                }
            });
        };
        auto cost = [&](const Rig& gg, const std::vector<RigFrame>& f) {
            std::vector<double> c(nf, 0.0);
            cv::parallel_for_(cv::Range(0, nf), [&](const cv::Range& rg) {
                std::vector<double> res;
                for (int i = rg.start; i < rg.end; ++i) {
                    residuals(gg, f[i], f[i].r, f[i].t, W[i], res);
                    for (double x : res)
                        c[i] += x * x;
                }
            });
            double s = 0;
            for (double x : c)
                s += x;
            return s;
        };
        using MG = cv::Matx<double, NG, NG>;
        using VG = cv::Matx<double, NG, 1>;
        using WG = cv::Matx<double, NG, 6>;
        std::vector<MG> Ui(nf);
        std::vector<VG> bgi(nf);
        std::vector<cv::Matx66d> V(nf);
        std::vector<WG> Wm(nf);
        std::vector<cv::Matx61d> bp(nf);
        double lambda = 1e-3;
        for (int it = 0; it < iters; ++it) {
            if (onIter)
                onIter(it);
            weights();
            const double c0 = cost(g, fr);
            cv::parallel_for_(cv::Range(0, nf), [&](const cv::Range& rg) {
                std::vector<double> r0, r1;
                for (int i = rg.start; i < rg.end; ++i) {
                    const RigFrame& f = fr[i];
                    residuals(g, f, f.r, f.t, W[i], r0);
                    const int nr = int(r0.size());
                    std::vector<std::array<double, 6>> Jp(nr);
                    std::vector<std::array<double, NG>> Jg(nr);
                    for (int j = 0; j < 6; ++j) {
                        cv::Vec3d r = f.r, t = f.t;
                        const double h = j < 3 ? 1e-7 : 1e-5;
                        (j < 3 ? r[j] : t[j - 3]) += h;
                        residuals(g, f, r, t, W[i], r1);
                        for (int k = 0; k < nr; ++k)
                            Jp[k][j] = (r1[k] - r0[k]) / h;
                    }
                    for (int j = 0; j < NG; ++j) {
                        Rig gg = g;
                        double& p = GParam(gg, j);
                        const double h = j < 2 * NI ? 1e-6 * std::max(1.0, std::fabs(p)) : 1e-7;
                        p += h;
                        residuals(gg, f, f.r, f.t, W[i], r1);
                        for (int k = 0; k < nr; ++k)
                            Jg[k][j] = (r1[k] - r0[k]) / h;
                    }
                    Ui[i] = MG::zeros();
                    bgi[i] = VG::zeros();
                    V[i] = cv::Matx66d::zeros();
                    Wm[i] = WG::zeros();
                    bp[i] = cv::Matx61d::zeros();
                    for (int k = 0; k < nr; ++k) {
                        for (int a = 0; a < 6; ++a) {
                            bp[i](a) += Jp[k][a] * r0[k];
                            for (int c = 0; c < 6; ++c)
                                V[i](a, c) += Jp[k][a] * Jp[k][c];
                        }
                        for (int a = 0; a < NG; ++a) {
                            if (Jg[k][a] == 0)
                                continue;
                            bgi[i](a) += Jg[k][a] * r0[k];
                            for (int c = 0; c < NG; ++c)
                                Ui[i](a, c) += Jg[k][a] * Jg[k][c];
                            for (int c = 0; c < 6; ++c)
                                Wm[i](a, c) += Jg[k][a] * Jp[k][c];
                        }
                    }
                }
            });
            MG U = MG::zeros();
            VG bg = VG::zeros();
            for (int i = 0; i < nf; ++i) {
                U += Ui[i];
                bg += bgi[i];
            }
            bool stepped = false;
            for (int tries = 0; tries < 10 && !stepped; ++tries) {
                Rig g2 = g;
                std::vector<RigFrame> f2 = fr;
                std::vector<cv::Matx66d> Vinv(nf);
                MG S = U;
                VG rhs = -bg;
                for (int a = 0; a < NG; ++a)
                    S(a, a) *= 1 + lambda;
                for (int i = 0; i < nf; ++i) {
                    cv::Matx66d v = V[i];
                    for (int a = 0; a < 6; ++a)
                        v(a, a) *= 1 + lambda;
                    Vinv[i] = v.inv(cv::DECOMP_CHOLESKY);
                    const WG WV = Wm[i] * Vinv[i];
                    S -= WV * Wm[i].t();
                    rhs += WV * bp[i];
                }
                cv::Mat dg;
                cv::solve(cv::Mat(S), cv::Mat(rhs), dg, cv::DECOMP_SVD);
                VG d;
                for (int j = 0; j < NG; ++j) {
                    d(j) = dg.at<double>(j);
                    GParam(g2, j) += d(j);
                }
                for (int i = 0; i < nf; ++i) {
                    const cv::Matx61d dp = Vinv[i] * (-bp[i] - Wm[i].t() * d);
                    for (int a = 0; a < 3; ++a) {
                        f2[i].r[a] += dp(a);
                        f2[i].t[a] += dp(a + 3);
                    }
                }
                if (cost(g2, f2) < c0) {
                    g = g2;
                    fr = std::move(f2);
                    lambda = std::max(1e-7, lambda / 4);
                    stepped = true;
                } else {
                    lambda *= 8;
                }
            }
            if (!stepped)
                break;
        }
    }

    // RMS per lens over corners within `drop`, and each corner's angle off
    // its lens's axis (for the coverage).
    void report(const Rig& g, const std::vector<RigFrame>& fr, double drop, double rms[2],
                std::vector<double> theta[2]) const
    {
        double ss[2] = {0, 0};
        int n[2] = {0, 0};
        for (const RigFrame& f : fr) {
            const cv::Matx33d R = Rot(f.r), Rr = Rot(g.r);
            for (int l = 0; l < 2; ++l) {
                if (!f.f[l])
                    continue;
                for (const Obs& o : f.f[l]->obs) {
                    cv::Vec3d X = R * fit_.boardPoint(o.id) + f.t;
                    if (l == 1)
                        X = Rr * X + g.t;
                    double u, v;
                    if (!Project(g.m[l], X, u, v))
                        continue;
                    const double e = std::hypot(u - o.u, v - o.v);
                    if (e > drop)
                        continue;
                    ss[l] += e * e;
                    ++n[l];
                    theta[l].push_back(std::acos(std::clamp(X[2] / cv::norm(X), -1.0, 1.0)) / kDeg);
                }
            }
        }
        for (int l = 0; l < 2; ++l)
            rms[l] = std::sqrt(ss[l] / std::max(1, n[l]));
    }

    // Transfer: in frames both lenses saw, the board's pose from lens 0's
    // corners alone, then where lens 1's corners should be through the rig;
    // the median angle between that and where lens 1 sees them, degrees.
    double transfer(const Rig& g, const std::vector<RigFrame>& fr) const
    {
        std::vector<RigFrame> both;
        for (const RigFrame& rf : fr)
            if (rf.f[0] && rf.f[1])
                both.push_back(rf);
        std::vector<std::vector<double>> per(both.size());
        cv::parallel_for_(cv::Range(0, int(both.size())), [&](const cv::Range& rg) {
            for (int i = rg.start; i < rg.end; ++i) {
                const RigFrame& rf = both[i];
                std::vector<Frame> one{*rf.f[0]};
                one[0].r = rf.r;
                one[0].t = rf.t;
                one[0].ok = true;
                Intr m0 = g.m[0];
                fit_.adjust(m0, one, true, 10, 2.0, 1e9);
                const cv::Matx33d R = Rot(one[0].r), Rr = Rot(g.r);
                for (const Obs& o : rf.f[1]->obs) {
                    const cv::Vec3d X = Rr * (R * fit_.boardPoint(o.id) + one[0].t) + g.t;
                    cv::Vec3d seen;
                    if (!Unproject(g.m[1], o.u, o.v, seen))
                        continue;
                    per[i].push_back(
                        std::acos(std::clamp(seen.dot(X / cv::norm(X)), -1.0, 1.0)) / kDeg);
                }
            }
        });
        std::vector<double> errs;
        for (auto& v : per)
            errs.insert(errs.end(), v.begin(), v.end());
        if (errs.empty())
            return NAN;
        std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
        return errs[errs.size() / 2];
    }

private:
    const Fitter& fit_;
};

// The calibration's axes: ours with x and y negated.
cv::Matx33d CalibAxes(const insv::Mat3& worldToLens)
{
    cv::Matx33d A;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            A(a, b) = (a < 2 ? -1 : 1) * worldToLens.m[a][b];
    return A;
}

// Lens 0 -> lens 1 rotation of a stitch set up with `cal`.
cv::Matx33d RigRotation(const insv::InsvCalibration& cal, int w, int h)
{
    insv::FisheyeStitcher st;
    st.configure(cal, w, h, insv::StitchParams{});
    return CalibAxes(st.lensProjection(1).worldToLens) *
           CalibAxes(st.lensProjection(0).worldToLens).t();
}

// Lens 1's yaw / pitch / roll so the stitch's lens 0 -> lens 1 rotation is
// `target` (Gauss-Newton through the stitch's own set-up, so whatever its
// conventions are, the result means the same to it).
bool SolveLens1Rotation(insv::InsvCalibration& cal, int w, int h, const cv::Matx33d& target)
{
    double* a[3] = {&cal.lensesExt[1].yaw, &cal.lensesExt[1].pitch, &cal.lensesExt[1].roll};
    auto err = [&] {
        return RotVec(target * RigRotation(cal, w, h).t());
    };
    for (int it = 0; it < 30; ++it) {
        const cv::Vec3d e = err();
        if (cv::norm(e) < 1e-12)
            return true;
        cv::Matx33d J;
        for (int j = 0; j < 3; ++j) {
            *a[j] += 1e-4;
            const cv::Vec3d e2 = err();
            *a[j] -= 1e-4;
            for (int r = 0; r < 3; ++r)
                J(r, j) = (e2[r] - e[r]) / 1e-4;
        }
        const cv::Vec3d d = J.solve(-e, cv::DECOMP_SVD);
        for (int j = 0; j < 3; ++j)
            *a[j] += d[j];
    }
    return cv::norm(err()) < 1e-9;
}

} // namespace

LensFitResult FitLensCalibration(const camera::ClipMetadata& clip, const ChartObservations& obs,
                                 const LensFitOptions& opt,
                                 const std::function<void(double)>& progress,
                                 const std::atomic<bool>* cancel)
{
    LensFitResult out;
    auto fail = [&out](std::string m) {
        out.ok = false;
        out.message = std::move(m);
        return out;
    };
    auto cancelled = [cancel] { return cancel && cancel->load(); };
    auto report = [&progress](double p) {
        if (progress)
            progress(std::clamp(p, 0.0, 1.0));
    };
    if (clip.lenses.size() != 2 || !clip.calibrated)
        return fail("the clip has no two-lens calibration");
    if (!(opt.squareMm > 0))
        return fail("no square size");
    if (obs.lensWidth != clip.lensWidth || obs.lensHeight != clip.lensHeight)
        return fail("the corners are from another picture size");

    insv::InsvCalibration fileCal;
    fileCal.lensesExt = {clip.lenses[0].calibration, clip.lenses[1].calibration};
    insv::FisheyeStitcher st;
    if (!st.configure(fileCal, clip.lensWidth, clip.lensHeight, insv::StitchParams{}))
        return fail("the file's calibration can't be set up");
    // The file's model as the app stitches it: the guard's profile on top.
    insv::FisheyeStitcher stGuard;
    const insv::FisheyeStitcher* acc = nullptr;
    {
        const std::optional<insv::LensProfile>& prof = opt.lensGuard == 1   ? clip.standardGuard
                                                       : opt.lensGuard == 2 ? clip.premiumGuard
                                                                            : std::nullopt;
        insv::StitchParams pg;
        if (prof && clip.bareLens && insv::ApplyLensProfile(pg, &*clip.bareLens, &*prof) &&
            pg.accessory && stGuard.configure(fileCal, clip.lensWidth, clip.lensHeight, pg))
            acc = &stGuard;
    }

    const Fitter fitter(opt.squareMm);
    Intr fitted[2];
    std::vector<Frame> lensFrames[2];
    for (int lens = 0; lens < 2; ++lens) {
        std::vector<Frame> frames;
        for (Frame& f : fitter.frames(obs.lens[lens]))
            if (f.motion <= opt.stillPxPerFrame && int(f.obs.size()) >= opt.minCorners)
                frames.push_back(std::move(f));
        // The fit starts from the bare file model; the poses from the
        // file's as stitched (with the guard), the nearer of the two.
        const Intr bare = FromProjection(st.lensProjection(lens));
        Intr file = bare;
        file.acc = acc;
        for (Frame& f : frames)
            f.ok = fitter.initPose(file, f);
        std::erase_if(frames, [](const Frame& f) { return !f.ok; });
        out.framesUsed[lens] = int(frames.size());
        if (frames.size() < 20)
            return fail("lens " + std::to_string(lens) + " saw the still chart in too few frames");
        if (cancelled())
            return fail("cancelled");
        // The file's model: poses only.
        Intr m = file;
        fitter.adjust(m, frames, true, 15, 2.0, 1e9);
        out.fileRmsPx[lens] = fitter.adjust(m, frames, true, 10, 2.0, 15.0);
        report(0.05 + 0.15 * lens);
        if (cancelled())
            return fail("cancelled");
        // Free.
        Intr g = bare;
        fitter.adjust(g, frames, false, 30, 1.0, 1e9);
        fitter.adjust(g, frames, false, 30, 1.0, 5.0);
        fitted[lens] = g;
        lensFrames[lens] = std::move(frames);
        report(0.15 + 0.15 * lens);
    }

    // Joint: the file's rig to start (lens 1's centre on lens 0's axis, the
    // spacing behind it), held in translation.
    const cv::Matx33d Rfile = RigRotation(fileCal, clip.lensWidth, clip.lensHeight);
    const cv::Vec3d cFile(0, 0, -st.baselineM() * 1000);
    std::map<int, RigFrame> byFrame;
    for (int l = 0; l < 2; ++l)
        for (const Frame& f : lensFrames[l]) {
            RigFrame& rf = byFrame[f.frame];
            rf.frame = f.frame;
            rf.f[l] = &f;
        }
    auto seed = [&](const Rig& g) {
        std::vector<RigFrame> fr;
        const cv::Matx33d Rr = Rot(g.r);
        for (auto& [k, rf] : byFrame) {
            RigFrame x = rf;
            if (x.f[0]) {
                x.r = x.f[0]->r;
                x.t = x.f[0]->t;
            } else {
                x.r = RotVec(Rr.t() * Rot(x.f[1]->r));
                x.t = Rr.t() * (x.f[1]->t - g.t);
            }
            fr.push_back(x);
        }
        return fr;
    };
    for (auto& [k, rf] : byFrame)
        out.framesBoth += rf.f[0] && rf.f[1];

    const RigFitter rig(fitter);
    Rig gf;
    gf.m[0] = FromProjection(st.lensProjection(0));
    gf.m[1] = FromProjection(st.lensProjection(1));
    gf.m[0].acc = gf.m[1].acc = acc;
    gf.r = RotVec(Rfile);
    gf.t = -(Rfile * cFile);
    {
        std::vector<RigFrame> fr = seed(gf);
        out.fileTransferDeg = rig.transfer(gf, fr);
    }
    if (cancelled())
        return fail("cancelled");
    report(0.35);

    Rig g = gf;
    g.m[0] = fitted[0];
    g.m[1] = fitted[1];
    std::vector<RigFrame> fr = seed(g);
    const int iters = 25;
    rig.adjust(g, fr, iters, 1.0, 1e9, [&](int it) { report(0.35 + 0.25 * it / iters); });
    if (cancelled())
        return fail("cancelled");
    rig.adjust(g, fr, iters, 1.0, 5.0, [&](int it) { report(0.6 + 0.3 * it / iters); });
    if (cancelled())
        return fail("cancelled");

    stitch::LensCalibration& cal = out.calibration;
    cal.squareMm = opt.squareMm;
    std::vector<double> theta[2];
    rig.report(g, fr, 5.0, cal.rmsPx, theta);
    cal.transferDeg = rig.transfer(g, fr);
    // Coverage: the furthest 5-degree ring with a fair number of corners.
    for (int l = 0; l < 2; ++l) {
        int count[40] = {};
        for (double t : theta[l])
            ++count[std::clamp(int(t / 5), 0, 39)];
        for (int b = 39; b >= 0; --b)
            if (count[b] >= 30) {
                cal.coverageDeg[l] = 5.0 * (b + 1);
                break;
            }
    }
    report(0.95);

    // Back into the file's units and the stitch's rotation for lens 1.
    insv::InsvCalibration fitCal = fileCal;
    for (int l = 0; l < 2; ++l) {
        insv::LensCalibExt& e = fitCal.lensesExt[l];
        const double halfW = e.w * 0.5;
        const double sCrop = double(clip.lensWidth) / e.focalRefW;
        const double sSensor = double(clip.lensWidth) / halfW;
        const double* p = g.m[l].p;
        e.fx = p[0] / sCrop;
        e.fy = p[1] / sCrop;
        e.cx = p[2] / sSensor + (l == 1 ? halfW : 0);
        e.cy = p[3] / sSensor;
        for (int k = 0; k < 4; ++k) {
            e.k[k] = p[4 + k];
            e.s[k] = p[10 + k];
        }
        e.p[0] = p[8];
        e.p[1] = p[9];
    }
    if (!SolveLens1Rotation(fitCal, clip.lensWidth, clip.lensHeight, Rot(g.r)))
        return fail("lens 1's rotation doesn't solve");
    cal.lenses = {fitCal.lensesExt[0], fitCal.lensesExt[1]};
    cal.fileLenses = {fileCal.lensesExt[0], fileCal.lensesExt[1]};
    {
        const cv::Vec3d d = RotVec(Rfile.t() * Rot(g.r));
        out.rotationChangeDeg = cv::norm(d) / kDeg;
    }

    // How far it moves the picture where the lenses meet: per lens, world
    // directions 90 deg off its axis in the file's set-up, through the
    // file's model to a pixel, back out through the new one.
    insv::FisheyeStitcher stFit;
    stFit.configure(fitCal, clip.lensWidth, clip.lensHeight, insv::StitchParams{});
    for (int l = 0; l < 2; ++l) {
        const cv::Matx33d Af = CalibAxes(st.lensProjection(l).worldToLens);
        const cv::Matx33d An = CalibAxes(stFit.lensProjection(l).worldToLens);
        Intr mf = FromProjection(st.lensProjection(l));
        mf.acc = acc;
        const Intr mn = FromProjection(stFit.lensProjection(l));
        std::vector<double> d;
        for (int k = 0; k < 360; ++k) {
            const double a = k * kDeg;
            const cv::Vec3d X(std::cos(a), std::sin(a), 0); // 90 deg off the axis
            double u, v;
            cv::Vec3d ray;
            if (!Project(mf, X, u, v) || !Unproject(mn, u, v, ray))
                continue;
            const cv::Vec3d w0 = Af.t() * X, w1 = An.t() * ray;
            d.push_back(std::acos(std::clamp(w0.dot(w1) / cv::norm(w1), -1.0, 1.0)) / kDeg);
        }
        if (d.empty())
            continue;
        std::sort(d.begin(), d.end());
        out.shiftDeg[l] = d[d.size() / 2];
        out.shiftMaxDeg[l] = d.back();
    }
    out.ok = true;
    report(1.0);
    return out;
}

std::vector<LensFitProblem> CheckLensFit(const LensFitResult& r)
{
    std::vector<LensFitProblem> p;
    const stitch::LensCalibration& c = r.calibration;
    if (r.framesUsed[0] < kMinFrames || r.framesUsed[1] < kMinFrames)
        p.push_back(LensFitProblem::TooFewFrames);
    if (c.coverageDeg[0] < kMinCoverageDeg)
        p.push_back(LensFitProblem::CoverageLens0);
    if (c.coverageDeg[1] < kMinCoverageDeg)
        p.push_back(LensFitProblem::CoverageLens1);
    if (!(c.rmsPx[0] <= kMaxRmsPx && c.rmsPx[1] <= kMaxRmsPx))
        p.push_back(LensFitProblem::Rms);
    if (!(c.transferDeg <= kMaxTransferDeg))
        p.push_back(LensFitProblem::Transfer);
    return p;
}

} // namespace calib
