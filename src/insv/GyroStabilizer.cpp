// SPDX-License-Identifier: MIT OR Apache-2.0
#include "GyroStabilizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace insv {

namespace {

// --- quaternion algebra (w,x,y,z; unit = body->world) -----------------------

struct Q {
    double w = 1, x = 0, y = 0, z = 0;
};

Q toQ(const std::array<double, 4>& a) { return {a[0], a[1], a[2], a[3]}; }
std::array<double, 4> fromQ(const Q& q) { return {q.w, q.x, q.y, q.z}; }

Q qmul(const Q& a, const Q& b)
{
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Q qconj(const Q& q) { return {q.w, -q.x, -q.y, -q.z}; }

double qdot(const Q& a, const Q& b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }

Q qnorm(Q q)
{
    const double n = std::sqrt(qdot(q, q));
    if (n < 1e-12)
        return {1, 0, 0, 0};
    return {q.w / n, q.x / n, q.y / n, q.z / n};
}

Q qneg(const Q& q) { return {-q.w, -q.x, -q.y, -q.z}; }

// exp map: rotation vector (axis * angle, rad) -> unit quaternion.
Q qexp(double rx, double ry, double rz)
{
    const double theta = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (theta < 1e-9)
        return qnorm({1.0, 0.5 * rx, 0.5 * ry, 0.5 * rz});
    const double s = std::sin(theta * 0.5) / theta;
    return {std::cos(theta * 0.5), rx * s, ry * s, rz * s};
}

Q qslerp(Q a, Q b, double t)
{
    double d = qdot(a, b);
    if (d < 0) {
        b = qneg(b);
        d = -d;
    }
    if (d > 0.9995)
        return qnorm({a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t});
    const double th0 = std::acos(d);
    const double th = th0 * t;
    const double s1 = std::sin(th) / std::sin(th0);
    const double s0 = std::cos(th) - d * s1;
    return {a.w * s0 + b.w * s1, a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1};
}

// Row-major matrix for `world = M v`.
void qToMat(const Q& q, double m[3][3])
{
    const double w = q.w, x = q.x, y = q.y, z = q.z;
    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - w * z);
    m[0][2] = 2 * (x * z + w * y);
    m[1][0] = 2 * (x * y + w * z);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - w * x);
    m[2][0] = 2 * (x * z - w * y);
    m[2][1] = 2 * (y * z + w * x);
    m[2][2] = 1 - 2 * (x * x + y * y);
}

// Rotation matrix (row-major, world = M v) -> unit quaternion (Shepperd).
Q matToQ(const double m[3][3])
{
    const double tr = m[0][0] + m[1][1] + m[2][2];
    Q q;
    if (tr > 0.0) {
        const double s = 2.0 * std::sqrt(1.0 + tr);
        q = {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
        q = {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    } else if (m[1][1] > m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
        q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
    } else {
        const double s = 2.0 * std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
        q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
    }
    return qnorm(q);
}

// Decompose to the renderer's convention:  M == Ry(yaw) Rx(pitch) Rz(roll).
void matYPR(const double m[3][3], double& yaw, double& pitch, double& roll)
{
    pitch = std::asin(std::clamp(-m[1][2], -1.0, 1.0));
    roll = std::atan2(m[1][0], m[1][1]);
    yaw = std::atan2(m[0][2], m[2][2]);
}

Q qFromYPR(double yaw, double pitch, double roll)
{
    const Q qy{std::cos(yaw / 2), 0, std::sin(yaw / 2), 0};
    const Q qx{std::cos(pitch / 2), std::sin(pitch / 2), 0, 0};
    const Q qz{std::cos(roll / 2), 0, 0, std::sin(roll / 2)};
    return qnorm(qmul(qmul(qy, qx), qz)); // Ry * Rx * Rz
}

void qDecomposeYPR(const Q& q, double& yaw, double& pitch, double& roll)
{
    double m[3][3];
    qToMat(q, m);
    matYPR(m, yaw, pitch, roll);
}

struct AxisMap {
    int idx[3] = {0, 1, 2};
    double sign[3] = {1, 1, 1};
};

AxisMap parseAxes(const std::string& s)
{
    AxisMap am;
    if (s.empty())
        return am;
    size_t start = 0;
    for (int k = 0; k < 3; ++k) {
        const size_t comma = s.find(',', start);
        std::string tok =
            s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? s.size() : comma + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.pop_back();
        double sgn = 1.0;
        if (!tok.empty() && (tok[0] == '-' || tok[0] == '+')) {
            sgn = tok[0] == '-' ? -1.0 : 1.0;
            tok.erase(tok.begin());
        }
        if (tok.size() != 1 || tok[0] < 'x' || tok[0] > 'z')
            return AxisMap{}; // malformed -> identity
        am.idx[k] = tok[0] - 'x';
        am.sign[k] = sgn;
    }
    return am;
}

Q sampleTrack(const std::vector<double>& ts, const std::vector<std::array<double, 4>>& qs, double t)
{
    if (qs.empty())
        return {};
    if (t <= ts.front())
        return toQ(qs.front());
    if (t >= ts.back())
        return toQ(qs.back());
    size_t lo = 0, hi = ts.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        (ts[mid] <= t ? lo : hi) = mid;
    }
    const double span = ts[hi] - ts[lo];
    const double f = span > 1e-9 ? (t - ts[lo]) / span : 0.0;
    return qslerp(toQ(qs[lo]), toQ(qs[hi]), f);
}

size_t lowerBound(const std::vector<double>& ts, double v)
{
    size_t a = 0, b = ts.size();
    while (a < b) {
        const size_t mid = (a + b) / 2;
        if (ts[mid] < v)
            a = mid + 1;
        else
            b = mid;
    }
    return a;
}

// Whole-clip gyro bias + gravity fit for horizon leveling. The X5's gyro keeps
// a residual bias of ~0.3-0.4 deg/s (measured 2026-10-09 on the stationary
// Modes clips); against build()'s deliberately weak accel anchor (kAccelGain)
// that settles as a steady tilt of up to bias/gain, 3-9 deg -- the constant
// roll Horizon lock showed on every clip. This fits the constant bias `b` that
// makes the measured up, carried into the world frame by the integrated
// (rate - b) track, stay put on the quiet samples `sel`; `gw` is that world
// up for a track started at identity. Levenberg-Marquardt over b with a small
// ridge: the component along gravity isn't observable and stays ~0.
struct BiasFit {
    bool ok = false;
    double b[3] = {0, 0, 0};
    double gw[3] = {0, 1, 0};
};

BiasFit fitGyroBias(const std::vector<std::array<double, 3>>& rate, const std::vector<double>& dt,
                    const std::vector<std::array<double, 3>>& up, const std::vector<size_t>& sel)
{
    BiasFit fit;
    const size_t m = sel.size();
    if (m < 50)
        return fit;
    // Sum the rate into ~10-sample blocks (rotation vector d, duration T):
    // bias enters each block as b*T, and the in-block coning the sum drops is
    // far below what leveling needs. Ten times fewer steps per evaluation.
    constexpr size_t kBlock = 10;
    const size_t nb = (rate.size() + kBlock - 1) / kBlock;
    std::vector<std::array<double, 4>> blk(nb, {0.0, 0.0, 0.0, 0.0}); // dx dy dz T
    for (size_t i = 1; i < rate.size(); ++i) {
        if (dt[i] <= 0.0 || dt[i] >= 0.2)
            continue;
        auto& bl = blk[i / kBlock];
        for (int a = 0; a < 3; ++a)
            bl[a] += rate[i][a] * dt[i];
        bl[3] += dt[i];
    }
    // World up (unit, R u) at each selected sample (taken at the end of its
    // block) for bias b, minus their mean; returns the cost, mean in g.
    auto eval = [&](const double b[3], std::vector<double>& r, double g[3]) {
        r.assign(3 * m, 0.0);
        Q q{1, 0, 0, 0};
        size_t k = 0;
        for (size_t j = 0; j < nb && k < m; ++j) {
            const auto& bl = blk[j];
            q = qnorm(qmul(q, Q{1.0, 0.5 * (bl[0] - b[0] * bl[3]), 0.5 * (bl[1] - b[1] * bl[3]),
                                0.5 * (bl[2] - b[2] * bl[3])}));
            if (k < m && sel[k] / kBlock == j) {
                double R[3][3];
                qToMat(q, R);
                for (; k < m && sel[k] / kBlock == j; ++k) {
                    const auto& u = up[sel[k]];
                    for (int a = 0; a < 3; ++a)
                        r[3 * k + a] = R[a][0] * u[0] + R[a][1] * u[1] + R[a][2] * u[2];
                }
            }
        }
        g[0] = g[1] = g[2] = 0.0;
        for (size_t j = 0; j < m; ++j)
            for (int a = 0; a < 3; ++a)
                g[a] += r[3 * j + a];
        for (int a = 0; a < 3; ++a)
            g[a] /= static_cast<double>(m);
        double cost = 0.0;
        for (size_t j = 0; j < m; ++j)
            for (int a = 0; a < 3; ++a) {
                r[3 * j + a] -= g[a];
                cost += r[3 * j + a] * r[3 * j + a];
            }
        return cost;
    };

    double b[3] = {0, 0, 0}, g[3];
    std::vector<double> r, rp;
    double cost = eval(b, r, g);
    double lambda = 1e-3;
    constexpr double kH = 1e-4; // rad/s, numeric-Jacobian step
    for (int it = 0; it < 12; ++it) {
        std::vector<double> J[3];
        for (int p = 0; p < 3; ++p) {
            double bp[3] = {b[0], b[1], b[2]}, gp[3];
            bp[p] += kH;
            eval(bp, rp, gp);
            J[p].resize(r.size());
            for (size_t j = 0; j < r.size(); ++j)
                J[p][j] = (rp[j] - r[j]) / kH;
        }
        double A[3][3] = {}, rhs[3] = {};
        for (int p = 0; p < 3; ++p) {
            for (int s = 0; s < 3; ++s)
                for (size_t j = 0; j < r.size(); ++j)
                    A[p][s] += J[p][j] * J[s][j];
            for (size_t j = 0; j < r.size(); ++j)
                rhs[p] -= J[p][j] * r[j];
        }
        const double ridge = 1e-4 * (A[0][0] + A[1][1] + A[2][2]) / 3.0 + 1e-12;
        bool improved = false;
        for (int tries = 0; tries < 6 && !improved; ++tries) {
            double M[3][3];
            for (int p = 0; p < 3; ++p)
                for (int s = 0; s < 3; ++s)
                    M[p][s] = A[p][s] + (p == s ? lambda * A[p][p] + ridge : 0.0);
            // 3x3 solve by Cramer's rule.
            const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                               M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                               M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
            if (std::fabs(det) < 1e-300)
                break;
            double d[3];
            for (int c = 0; c < 3; ++c) {
                double Mc[3][3];
                for (int p = 0; p < 3; ++p)
                    for (int s = 0; s < 3; ++s)
                        Mc[p][s] = s == c ? rhs[p] : M[p][s];
                d[c] = (Mc[0][0] * (Mc[1][1] * Mc[2][2] - Mc[1][2] * Mc[2][1]) -
                        Mc[0][1] * (Mc[1][0] * Mc[2][2] - Mc[1][2] * Mc[2][0]) +
                        Mc[0][2] * (Mc[1][0] * Mc[2][1] - Mc[1][1] * Mc[2][0])) /
                       det;
            }
            const double bn[3] = {b[0] + d[0], b[1] + d[1], b[2] + d[2]};
            double gn[3];
            const double cn = eval(bn, rp, gn);
            if (cn < cost) {
                const double rel = (cost - cn) / std::max(cost, 1e-300);
                b[0] = bn[0], b[1] = bn[1], b[2] = bn[2];
                cost = cn;
                r.swap(rp);
                g[0] = gn[0], g[1] = gn[1], g[2] = gn[2];
                lambda = std::max(lambda * 0.3, 1e-6);
                improved = true;
                if (rel < 1e-6)
                    it = 99; // converged
            } else {
                lambda *= 10.0;
            }
        }
        if (!improved)
            break;
    }
    // A real residual bias is well under 1 deg/s; anything larger is the fit
    // chasing non-gravity accel, so don't trust it.
    if (std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]) > 0.02)
        return fit;
    const double gn = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (gn < 0.5)
        return fit;
    fit.ok = true;
    for (int a = 0; a < 3; ++a) {
        fit.b[a] = b[a];
        fit.gw[a] = g[a] / gn;
    }
    return fit;
}

// Three box blurs of width w (~ a Gaussian of sigma w/2), in place; edges
// sum only what's there. Prefix sums, so O(n) whatever the width.
void boxBlur3(std::vector<std::array<double, 4>>& v, size_t w)
{
    const size_t n = v.size();
    if (n < 2 || w < 2)
        return;
    std::vector<std::array<double, 4>> pre(n + 1, {0.0, 0.0, 0.0, 0.0});
    for (int pass = 0; pass < 3; ++pass) {
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < 4; ++k)
                pre[i + 1][k] = pre[i][k] + v[i][k];
        for (size_t i = 0; i < n; ++i) {
            const size_t lo = i >= w / 2 ? i - w / 2 : 0;
            const size_t hi = std::min(n, i + w / 2 + 1);
            for (int k = 0; k < 4; ++k)
                v[i][k] = pre[hi][k] - pre[lo][k];
        }
    }
}

// Level an integrated track against gravity, non-causally. The world up the
// accel shows on the quiet samples, w = R(t) u(t), would be a constant +Y if
// the track were exact; residual gyro bias (it drifts over a long ride) makes
// it wander slowly. Smooth w over +-kLevelSigmaSec (zero-phase, so no
// start-up transient and no lag), drop samples more than kOutlierDeg off that
// (braking, a lean the turn gate missed) and smooth again, then turn the
// track at each time by the small world rotation that takes the smoothed up
// onto +Y. Gaps with no quiet samples hold the clip-wide mean.
void levelTrack(const std::vector<double>& tSec, std::vector<std::array<double, 4>>& qTrack,
                const std::vector<std::array<double, 3>>& up, const std::vector<char>& quiet)
{
    constexpr double kBinSec = 0.1;
    constexpr double kLevelSigmaSec = 10.0;
    constexpr double kOutlierDeg = 5.0;
    const size_t n = qTrack.size();
    if (n < 2 || up.size() != n || quiet.size() != n)
        return;
    const size_t nb = static_cast<size_t>(tSec.back() / kBinSec) + 1;
    auto bin = [&](size_t i) { return std::min(nb - 1, static_cast<size_t>(tSec[i] / kBinSec)); };
    std::vector<std::array<double, 3>> w(n, {0.0, 1.0, 0.0});
    for (size_t i = 0; i < n; ++i) {
        if (!quiet[i])
            continue;
        double R[3][3];
        qToMat(toQ(qTrack[i]), R);
        for (int a = 0; a < 3; ++a)
            w[i][a] = R[a][0] * up[i][0] + R[a][1] * up[i][1] + R[a][2] * up[i][2];
    }
    // Three boxes of width 2 sigma make a Gaussian of ~sigma (variance 3 w^2/12).
    const size_t width = static_cast<size_t>(2.0 * kLevelSigmaSec / kBinSec);
    std::vector<std::array<double, 3>> g(nb, {0.0, 1.0, 0.0});
    const double cosOut = std::cos(kOutlierDeg * std::numbers::pi / 180.0);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::array<double, 4>> acc(nb, {0.0, 0.0, 0.0, 0.0});
        double tot[3] = {0, 0, 0};
        for (size_t i = 0; i < n; ++i) {
            if (!quiet[i])
                continue;
            const size_t b = bin(i);
            if (pass == 1 && w[i][0] * g[b][0] + w[i][1] * g[b][1] + w[i][2] * g[b][2] < cosOut)
                continue;
            for (int a = 0; a < 3; ++a) {
                acc[b][a] += w[i][a];
                tot[a] += w[i][a];
            }
            acc[b][3] += 1.0;
        }
        const double tn = std::sqrt(tot[0] * tot[0] + tot[1] * tot[1] + tot[2] * tot[2]);
        if (tn < 1e-9)
            return;
        boxBlur3(acc, width);
        // The clip mean, weighted as ~1 s of samples: only matters in gaps.
        const double eps = 1000.0;
        for (size_t b = 0; b < nb; ++b) {
            double v[3];
            for (int a = 0; a < 3; ++a)
                v[a] = acc[b][a] + eps * tot[a] / tn;
            const double vn = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            g[b] = {v[0] / vn, v[1] / vn, v[2] / vn};
        }
    }
    for (size_t i = 0; i < n; ++i) {
        // Linear between bin centres.
        const double x = std::clamp(tSec[i] / kBinSec - 0.5, 0.0, static_cast<double>(nb - 1));
        const size_t b0 = static_cast<size_t>(x), b1 = std::min(nb - 1, b0 + 1);
        const double f = x - static_cast<double>(b0);
        double u0[3];
        for (int a = 0; a < 3; ++a)
            u0[a] = g[b0][a] * (1.0 - f) + g[b1][a] * f;
        const double un = std::sqrt(u0[0] * u0[0] + u0[1] * u0[1] + u0[2] * u0[2]);
        // axis = u0 x +Y, angle = acos(u0 . +Y): takes u0 onto +Y.
        const double ax = -u0[2] / un, az = u0[0] / un;
        const double sn = std::sqrt(ax * ax + az * az);
        if (sn < 1e-12)
            continue;
        const double ang = std::atan2(sn, u0[1] / un);
        qTrack[i] = fromQ(qnorm(qmul(qexp(ax / sn * ang, 0.0, az / sn * ang), toQ(qTrack[i]))));
    }
}

} // namespace

namespace {
// Strength slider (0..1) <-> smoothing window: a power curve from minSec to
// maxSec whose exponent puts 0.5 exactly on midSec (the mode's default).
struct StrengthCurve {
    float minSec, midSec, maxSec;
    float exponent() const { return std::log((midSec - minSec) / (maxSec - minSec)) / std::log(0.5f); }
    float window(float s) const
    {
        s = std::clamp(s, 0.0f, 1.0f);
        return minSec + (maxSec - minSec) * std::pow(s, exponent());
    }
    float strength(float w) const
    {
        const float f = (std::clamp(w, minSec, maxSec) - minSec) / (maxSec - minSec);
        return std::clamp(std::pow(f, 1.0f / exponent()), 0.0f, 1.0f);
    }
};
constexpr StrengthCurve kLeanCurve{0.20f, 0.90f, 5.0f};    // Lean follow, shared window
constexpr StrengthCurve kHorizonCurve{0.30f, 1.50f, 10.0f}; // Horizon lock heading
} // namespace

float leanWindowFromStrength(float s) { return kLeanCurve.window(s); }
float leanStrengthFromWindow(float w) { return kLeanCurve.strength(w); }
float horizonWindowFromStrength(float s) { return kHorizonCurve.window(s); }
float horizonStrengthFromWindow(float w) { return kHorizonCurve.strength(w); }

StabConfig StabConfig::preset(StabMode m)
{
    StabConfig c;
    c.mode = m;
    switch (m) {
    case StabMode::LeanFollow:
        // Strength 0.5 (~0.9 s): removes the fast/mid chassis+road shake; a
        // cornering lean plays out over 2-4 s so it passes through essentially
        // untouched (measured: <=3 deg roll correction against a ~40 deg lean).
        // The Stabilization panel's Strength slider retunes this window.
        c.windowYawSec = c.windowPitchSec = c.windowRollSec = leanWindowFromStrength(0.5f);
        c.horizonLockAmount = 0.0f;
        break;
    case StabMode::HorizonLock:
        c.windowYawSec = c.windowPitchSec = 1.5f;
        c.windowRollSec = 2.5f;
        c.horizonLockAmount = 1.0f;
        // The stick mount leaks ~0.75 g RMS of ~9 Hz resonance onto the accel;
        // per-sample normalisation of that turns it into both a DC bias and
        // broadband jitter in the leveled orientation. A ~1.5 Hz pre-filter on
        // the accel keeps only gravity. Measured on the Mälaren Runt clip: the
        // in-band [0.3-14 Hz] correction goes from amplifying the shake (ratio
        // 1.21) to attenuating it (0.83).
        c.accelLowPassHz = 1.5f;
        break;
    case StabMode::Custom:
    case StabMode::Off:
        break;
    }
    return c;
}

void GyroStabilizer::build(const ImuTrack& track, const StabConfig& cfg, double syncAdjustSec)
{
    StabConfig c = cfg;
    if (c.gyroAxes.empty())
        c.gyroAxes = track.axisMap;
    // An explicit non-zero offset is an override; otherwise the track's own.
    if (cfg.timeOffsetSec == 0.0f && track.hasOffset && !track.samples.empty())
        c.timeOffsetSec = static_cast<float>(track.offsetSec + syncAdjustSec);
    build(track.samples, c);
    m_clockRate = track.clockRate;
    m_readoutSec = track.readoutSec;
}

void GyroStabilizer::build(const std::vector<insv::ImuSample>& imu, const StabConfig& cfg)
{
    m_clockRate = 0.0; // build(ImuTrack, ...) sets it after this returns
    m_readoutSec = 0.0;
    if (cfg.mode == StabMode::Off || imu.size() < 2) {
        m_ready = false;
        m_cfg = cfg;
        m_t.clear();
        m_qRaw.clear();
        return;
    }

    const bool fuse = cfg.horizonLockAmount > 0.0f;
    const auto [lo, hi] = std::minmax_element(
        imu.begin(), imu.end(),
        [](const insv::ImuSample& a, const insv::ImuSample& b) { return a.timestamp < b.timestamp; });
    TrackCache& tc = m_cache;
    if (tc.valid && tc.fuse == fuse && tc.n == imu.size() && tc.tFirst == lo->timestamp &&
        tc.tLast == hi->timestamp && tc.axes == cfg.gyroAxes && tc.scale == cfg.gyroScale &&
        tc.lowPassHz == cfg.accelLowPassHz && tc.gain == cfg.horizonGain) {
        buildFromOrientation(tc.t, tc.q, cfg); // resets the two below
        m_levelFwd = tc.levelFwd;
        m_mountLevel = tc.mountLevel;
        return;
    }

    std::vector<insv::ImuSample> s = imu;
    std::sort(s.begin(), s.end(), [](const insv::ImuSample& a, const insv::ImuSample& b) {
        return a.timestamp < b.timestamp;
    });

    const AxisMap am = parseAxes(cfg.gyroAxes);
    const double t0 = static_cast<double>(s.front().timestamp);
    const double tScale = 1e-6; // ImuSample timestamps are microseconds

    // Body-frame gyro rate (axis-mapped) at sample i; 0.5*(prev+cur), rad/s,
    // minus the fitted residual bias (horizon leveling only; see fitGyroBias).
    double gyroBias[3] = {0, 0, 0};
    auto gyroRate = [&](size_t i, double& wx, double& wy, double& wz) {
        double g0[3], g1[3];
        for (int k = 0; k < 3; ++k) {
            g0[k] = am.sign[k] * s[i - 1].gyro[am.idx[k]];
            g1[k] = am.sign[k] * s[i].gyro[am.idx[k]];
        }
        wx = 0.5 * (g0[0] + g1[0]) * cfg.gyroScale - gyroBias[0];
        wy = 0.5 * (g0[1] + g1[1]) * cfg.gyroScale - gyroBias[1];
        wz = 0.5 * (g0[2] + g1[2]) * cfg.gyroScale - gyroBias[2];
    };
    // Axis-mapped accelerometer at sample i (body frame, g). Returns false when
    // the specific force isn't ~1 g (hard accel / braking / cornering) or the
    // track has no accel at all -- callers then coast on the gyro.
    //
    // cfg.accelLowPassHz > 0 runs a one-pole IIR over the axis-mapped accel
    // first (state below), so the ~11 Hz stick-resonance junk (0.4-0.6 g) never
    // reaches the Mahony term -- only the ~DC gravity
    // component survives to define "level". `dt` is the sample step; the pole is
    // exact for that step so an irregular IMU cadence stays a true `fc` cutoff.
    constexpr double kTwoPi = 6.283185307179586;
    const double accelFc = std::max(0.0f, cfg.accelLowPassHz);
    double aLP[3] = {0, 0, 0};
    bool aLPInit = false;
    auto accelUp = [&](size_t i, double dt, double u[3], double* mag = nullptr) -> bool {
        double a[3];
        for (int k = 0; k < 3; ++k)
            a[k] = am.sign[k] * s[i].accel[am.idx[k]];
        if (accelFc > 0.0 && dt > 0.0) {
            if (!aLPInit) {
                aLP[0] = a[0];
                aLP[1] = a[1];
                aLP[2] = a[2];
                aLPInit = true;
            } else {
                const double alpha = 1.0 - std::exp(-kTwoPi * accelFc * dt);
                for (int k = 0; k < 3; ++k)
                    aLP[k] += alpha * (a[k] - aLP[k]);
            }
            a[0] = aLP[0];
            a[1] = aLP[1];
            a[2] = aLP[2];
        }
        const double m = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (mag)
            *mag = m;
        if (m < 0.6 || m > 1.4)
            return false;
        u[0] = a[0] / m;
        u[1] = a[1] / m;
        u[2] = a[2] / m;
        return true;
    };

    // Gyro integration, plus -- only for horizon leveling -- a weak Mahony
    // accel term that keeps the world frame's +Y on gravity. Measured
    // 2026-09-25 on the stick-mount clip:
    //   - LeanFollow works on relative deltas and needs no "up" at all; the
    //     old always-on 1 rad/s accel term injected the ~9 Hz stick resonance
    //     and cost 26 % more residual rotation. So: pure gyro unless leveling.
    //   - For leveling, a strong term makes the horizon chase the accel, which
    //     in a coordinated turn points along the *leaned* bike, not gravity --
    //     every corner dragged the horizon. The gain is now a slow anchor (~20
    //     s time constant), frozen while turning (sustained world yaw rate) or
    //     when the specific force isn't ~1 g, and the track is seeded level
    //     from the first half second of accel instead of converging from
    //     identity. Studio behaves the same way (the since-removed tools/horizon_spike).
    constexpr double kAccelGain = 0.05;     // rad/s per rad of tilt error at horizonGain 1
    constexpr double kTurnYawRate = 0.08;   // rad/s (~4.6 deg/s) of low-passed world yaw
    constexpr double kTurnLpHz = 0.5;       // yaw-rate low-pass for the turn detector
    constexpr double kOneGTolerance = 0.12; // |a| within 1 +- this to count as gravity
    constexpr double kTurnMarginSec = 1.5;  // quiet samples keep this far from a turn

    std::vector<double> tSec;
    std::vector<std::array<double, 4>> qTrack;
    tSec.reserve(s.size());
    qTrack.reserve(s.size());

    // Whole-clip bias + gravity fit (fitGyroBias): on the quiet samples --
    // low-passed accel within 1 g +- kOneGTolerance and not turning (the
    // low-passed rate about the measured up, which needs no orientation) --
    // up to ~4000 spread evenly over the clip.
    bool fitted = false;
    double fitUp[3] = {0, 1, 0};
    // The quiet samples: low-passed accel within 1 g +- kOneGTolerance and not
    // turning (the low-passed rate about the measured up, which needs no
    // orientation), with a kTurnMarginSec margin around every turn -- corner
    // entry and exit lean the bike before and after the yaw rate shows it.
    const size_t n = s.size();
    std::vector<std::array<double, 3>> rate, up;
    std::vector<double> dts;
    std::vector<char> quiet;
    size_t nQuiet = 0;
    if (fuse) {
        rate.assign(n, {0.0, 0.0, 0.0});
        up.assign(n, {0.0, 1.0, 0.0});
        dts.assign(n, 0.0);
        std::vector<char> ok(n, 0), turn(n, 0);
        const double fc = accelFc > 0.0 ? accelFc : 1.5;
        double lp[3] = {0, 0, 0};
        double yawRateLp = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double a[3];
            for (int k = 0; k < 3; ++k)
                a[k] = am.sign[k] * s[i].accel[am.idx[k]];
            if (i == 0) {
                lp[0] = a[0], lp[1] = a[1], lp[2] = a[2];
                continue;
            }
            const double dt = (static_cast<double>(s[i].timestamp) -
                               static_cast<double>(s[i - 1].timestamp)) * tScale;
            dts[i] = dt;
            if (dt <= 0.0 || dt >= 0.2)
                continue;
            const double alpha = 1.0 - std::exp(-kTwoPi * fc * dt);
            for (int k = 0; k < 3; ++k)
                lp[k] += alpha * (a[k] - lp[k]);
            gyroRate(i, rate[i][0], rate[i][1], rate[i][2]);
            const double mg = std::sqrt(lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]);
            if (mg < 1e-6)
                continue;
            up[i] = {lp[0] / mg, lp[1] / mg, lp[2] / mg};
            const double yr = rate[i][0] * up[i][0] + rate[i][1] * up[i][1] + rate[i][2] * up[i][2];
            yawRateLp += (1.0 - std::exp(-kTwoPi * kTurnLpHz * dt)) * (yr - yawRateLp);
            ok[i] = std::fabs(mg - 1.0) < kOneGTolerance;
            turn[i] = std::fabs(yawRateLp) >= kTurnYawRate;
        }
        // Dilate the turns by kTurnMarginSec each way (two sweeps).
        quiet.assign(n, 0);
        const double margin = kTurnMarginSec / tScale;
        double lastTurn = -1e300;
        for (size_t i = 0; i < n; ++i) {
            const double ts = static_cast<double>(s[i].timestamp);
            if (turn[i])
                lastTurn = ts;
            quiet[i] = ok[i] && ts - lastTurn > margin;
        }
        double nextTurn = 1e300;
        for (size_t i = n; i-- > 0;) {
            const double ts = static_cast<double>(s[i].timestamp);
            if (turn[i])
                nextTurn = ts;
            if (nextTurn - ts <= margin)
                quiet[i] = 0;
            nQuiet += quiet[i] ? 1 : 0;
        }
    }

    if (fuse) {
        std::vector<size_t> sel;
        const size_t stride = std::max<size_t>(1, nQuiet / 4000);
        size_t c = 0;
        for (size_t i = 0; i < n; ++i)
            if (quiet[i] && (c++ % stride) == 0)
                sel.push_back(i);
        const BiasFit fit = fitGyroBias(rate, dts, up, sel);
        if (fit.ok) {
            fitted = true;
            for (int k = 0; k < 3; ++k) {
                gyroBias[k] = fit.b[k];
                fitUp[k] = fit.gw[k];
            }
        }
    }
    // With the fit, the track is levelled afterwards (levelTrack) and the
    // causal Mahony anchor below is off; it stays as the fallback.
    const double kp = fuse && !fitted ? kAccelGain * std::max(0.0f, cfg.horizonGain) : 0.0;

    // Seed: rotate the world up onto +Y so the track starts level -- the
    // whole-clip fit's up when it succeeded, else the mean body "up" over the
    // first 0.5 s (the stick resonance averages out over 0.5 s).
    Q q{1, 0, 0, 0};
    if (fitted) {
        const double* u0 = fitUp;
        // axis = u0 x +Y, angle = acos(u0 . +Y): takes u0 onto +Y.
        const double ax = -u0[2], az = u0[0];
        const double sn = std::sqrt(ax * ax + az * az);
        const double ang = std::atan2(sn, u0[1]);
        if (sn > 1e-9)
            q = qexp(ax / sn * ang, 0.0, az / sn * ang);
    } else {
        double a0[3] = {0, 0, 0};
        for (size_t i = 0; i < s.size(); ++i) {
            if ((static_cast<double>(s[i].timestamp) - t0) * tScale > 0.5)
                break;
            for (int k = 0; k < 3; ++k)
                a0[k] += am.sign[k] * s[i].accel[am.idx[k]];
        }
        const double n = std::sqrt(a0[0] * a0[0] + a0[1] * a0[1] + a0[2] * a0[2]);
        if (n > 1e-9) {
            const double u0[3] = {a0[0] / n, a0[1] / n, a0[2] / n};
            // axis = u0 x +Y, angle = acos(u0 . +Y): takes u0 onto +Y.
            const double ax = -u0[2], az = u0[0];
            const double sn = std::sqrt(ax * ax + az * az);
            const double ang = std::atan2(sn, u0[1]);
            if (sn > 1e-9)
                q = qexp(ax / sn * ang, 0.0, az / sn * ang);
        }
    }
    tSec.push_back(0.0);
    qTrack.push_back(fromQ(q));

    // Mean specific force in the body frame: the mount's average "up". On a
    // bike it's the bike's vertical axis even through corners (a coordinated
    // turn's specific force points along the leaned bike), so it levels Lean
    // follow (m_mountLevel) and picks the heading the level camera follows.
    // A plain vector mean of the raw accel: per-sample gating of the ~0.75 g
    // stick resonance would bias it.
    double sumAccel[3] = {0, 0, 0};
    long accelN = 0;
    double yawLp = 0.0; // low-passed world yaw rate, for the turn freeze

    for (size_t i = 1; i < s.size(); ++i) {
        const double t = (static_cast<double>(s[i].timestamp) - t0) * tScale;
        const double dt =
            (static_cast<double>(s[i].timestamp) - static_cast<double>(s[i - 1].timestamp)) * tScale;

        if (dt > 0 && dt < 0.2) {
            double wx, wy, wz;
            gyroRate(i, wx, wy, wz);

            double u[3]; // measured up, body frame (unit)
            double mag = 0.0;
            const bool haveUp = accelUp(i, dt, u, &mag);
            for (int k = 0; k < 3; ++k)
                sumAccel[k] += am.sign[k] * s[i].accel[am.idx[k]];
            ++accelN;
            if (kp > 0.0) {
                double R[3][3];
                qToMat(q, R);
                const double worldYaw = R[1][0] * wx + R[1][1] * wy + R[1][2] * wz;
                yawLp += (1.0 - std::exp(-kTwoPi * kTurnLpHz * dt)) * (worldYaw - yawLp);
                const bool turning = std::fabs(yawLp) > kTurnYawRate;
                if (haveUp && !turning && std::fabs(mag - 1.0) < kOneGTolerance) {
                    const double v[3] = {R[1][0], R[1][1], R[1][2]}; // estimated up, body (R^T*(0,1,0))
                    // e = u x v : a body rate that rotates the estimated up toward
                    // the measured up (dv/dt ~ kp*(u - v)).
                    const double e[3] = {u[1] * v[2] - u[2] * v[1],
                                         u[2] * v[0] - u[0] * v[2],
                                         u[0] * v[1] - u[1] * v[0]};
                    wx += kp * e[0];
                    wy += kp * e[1];
                    wz += kp * e[2];
                }
            }
            q = qnorm(qmul(q, qexp(wx * dt, wy * dt, wz * dt))); // body-frame increment
        }
        tSec.push_back(t);
        qTrack.push_back(fromQ(q));
    }

    if (fitted)
        levelTrack(tSec, qTrack, up, quiet);

    // Level-camera heading reference: body forward (-Z) with the mount's mean
    // up removed, i.e. "forward" of the mount made horizontal. m_mountLevel
    // turns the view frame onto (that forward, the mean up).
    m_levelFwd = {0.0, 0.0, -1.0};
    m_mountLevel = {1.0, 0.0, 0.0, 0.0};
    const double nb = std::sqrt(sumAccel[0] * sumAccel[0] + sumAccel[1] * sumAccel[1] +
                                sumAccel[2] * sumAccel[2]);
    if (accelN > 0 && nb > 0.5 * static_cast<double>(accelN)) { // mean |a| ~1 g: a real accel track
        const double bu[3] = {sumAccel[0] / nb, sumAccel[1] / nb, sumAccel[2] / nb};
        const double fd = -bu[2]; // f . bu with f = (0, 0, -1)
        const double h[3] = {-fd * bu[0], -fd * bu[1], -1.0 - fd * bu[2]}; // f - (f.bu) bu
        const double nh = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
        if (nh > 0.1) { // the mount isn't looking straight up/down
            m_levelFwd = {h[0] / nh, h[1] / nh, h[2] / nh};
            const double* f = m_levelFwd.data();
            // Columns: right = up x back, up, back = -forward.
            const double right[3] = {-(bu[1] * f[2] - bu[2] * f[1]), -(bu[2] * f[0] - bu[0] * f[2]),
                                     -(bu[0] * f[1] - bu[1] * f[0])};
            const double M[3][3] = {{right[0], bu[0], -f[0]},
                                    {right[1], bu[1], -f[1]},
                                    {right[2], bu[2], -f[2]}};
            m_mountLevel = fromQ(matToQ(M));
        }
    }

    tc = {true, fuse, imu.size(), lo->timestamp, hi->timestamp, cfg.gyroAxes, cfg.gyroScale,
          cfg.accelLowPassHz, cfg.horizonGain, std::move(tSec), std::move(qTrack), m_levelFwd,
          m_mountLevel};
    buildFromOrientation(tc.t, tc.q, cfg); // resets both
    m_levelFwd = tc.levelFwd;
    m_mountLevel = tc.mountLevel;
}

void GyroStabilizer::buildFromOrientation(const std::vector<double>& tSec,
                                          const std::vector<std::array<double, 4>>& qBodyToWorld,
                                          const StabConfig& cfg)
{
    m_ready = false;
    m_cfg = cfg;
    m_clockRate = 0.0; // nominal video time; build(ImuTrack, ...) sets it after
    m_t.clear();
    m_qRaw.clear();
    // m_levelFwd / m_mountLevel are set by build() from the accel; a direct
    // caller (an orientation track) gets whatever the last build() left, so
    // reset them here and let build() set them after.
    m_levelFwd = {0.0, 0.0, -1.0};
    m_mountLevel = {1.0, 0.0, 0.0, 0.0};

    if (cfg.mode == StabMode::Off || tSec.size() < 2 || tSec.size() != qBodyToWorld.size())
        return;

    m_t = tSec;
    m_qRaw.reserve(qBodyToWorld.size());
    for (const auto& a : qBodyToWorld)
        m_qRaw.push_back(fromQ(qnorm(toQ(a)))); // defensive: an integrated track can drift off unit
    m_ready = true;
}

void GyroStabilizer::bodyRotation(double tSec, double dtSec, float r[3]) const
{
    r[0] = r[1] = r[2] = 0.0f;
    if (!m_ready)
        return;
    const double t = gyroTime(tSec);
    Q d = qmul(qconj(sampleTrack(m_t, m_qRaw, t)),
               sampleTrack(m_t, m_qRaw, t + dtSec * (1.0 + m_clockRate)));
    if (d.w < 0)
        d = qneg(d);
    // log map: rotation vector = axis * angle
    const double s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const double k = s > 1e-12 ? 2.0 * std::atan2(s, d.w) / s : 2.0;
    r[0] = static_cast<float>(d.x * k);
    r[1] = static_cast<float>(d.y * k);
    r[2] = static_cast<float>(d.z * k);
}

std::array<double, 4> GyroStabilizer::rawOrientation(double tSec) const
{
    if (!m_ready)
        return {1.0, 0.0, 0.0, 0.0};
    return fromQ(sampleTrack(m_t, m_qRaw, gyroTime(tSec)));
}

void GyroStabilizer::correctionMatrix(double tSec, float m[3][3]) const
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r][c] = (r == c) ? 1.0f : 0.0f;
    if (!m_ready)
        return;

    const double t = gyroTime(tSec);
    const Q qRef = sampleTrack(m_t, m_qRaw, t);

    // Per-axis smoothing: average each sample's delta-from-ref (small angles,
    // so Euler is safe there), each component with its own Gaussian window.
    const double wMax =
        std::max({m_cfg.windowYawSec, m_cfg.windowPitchSec, m_cfg.windowRollSec, 1e-3f});
    const double sigY = std::max(m_cfg.windowYawSec * 0.5f, 1e-3f);
    const double sigP = std::max(m_cfg.windowPitchSec * 0.5f, 1e-3f);
    const double sigR = std::max(m_cfg.windowRollSec * 0.5f, 1e-3f);

    double accY = 0, accP = 0, accR = 0, wY = 0, wP = 0, wR = 0;
    for (size_t i = lowerBound(m_t, t - wMax); i < m_t.size(); ++i) {
        const double dtt = m_t[i] - t;
        if (dtt > wMax)
            break;
        double dyaw, dpitch, droll;
        qDecomposeYPR(qmul(qconj(qRef), toQ(m_qRaw[i])), dyaw, dpitch, droll);
        if (std::fabs(dtt) <= m_cfg.windowYawSec) {
            const double w = std::exp(-0.5 * dtt * dtt / (sigY * sigY));
            accY += w * dyaw;
            wY += w;
        }
        if (std::fabs(dtt) <= m_cfg.windowPitchSec) {
            const double w = std::exp(-0.5 * dtt * dtt / (sigP * sigP));
            accP += w * dpitch;
            wP += w;
        }
        if (std::fabs(dtt) <= m_cfg.windowRollSec) {
            const double w = std::exp(-0.5 * dtt * dtt / (sigR * sigR));
            accR += w * droll;
            wR += w;
        }
    }
    const double mYaw = wY > 0 ? accY / wY : 0.0;
    const double mPitch = wP > 0 ? accP / wP : 0.0;
    const double mRoll = wR > 0 ? accR / wR : 0.0;

    Q qSmooth = qnorm(qmul(qRef, qFromYPR(mYaw, mPitch, mRoll)));
    // Auto-level: look at the sphere through the mount's mean gravity frame,
    // so a tilted mount reads level and only departures from its average
    // posture (a cornering lean) tilt the view.
    if (m_cfg.autoLevel)
        qSmooth = qnorm(qmul(qSmooth, toQ(m_mountLevel)));

    // Horizon lock: a level virtual camera. Its heading follows the camera's
    // own heading (m_levelFwd carried through the track, projected onto the
    // horizontal plane), Gaussian-smoothed over windowYawSec; its pitch and
    // roll are zero against world +Y, which build()'s gravity-seeded, weakly
    // accel-anchored integration keeps on "up". The reframe pose then looks
    // at a level sphere, so any view with zero roll has a level horizon, at
    // any yaw/pitch. Partial amounts slerp from the smoothed follow
    // orientation toward it.
    const double amount = std::clamp(m_cfg.horizonLockAmount, 0.0f, 1.0f);
    if (amount > 0.0) {
        auto heading = [this](const Q& q, double fallback) {
            double R[3][3];
            qToMat(q, R);
            const double* h = m_levelFwd.data();
            const double fx = R[0][0] * h[0] + R[0][1] * h[1] + R[0][2] * h[2];
            const double fz = R[2][0] * h[0] + R[2][1] * h[1] + R[2][2] * h[2];
            if (fx * fx + fz * fz < 1e-6)
                return fallback; // pointing straight up/down: heading undefined
            return std::atan2(-fx, -fz); // Ry(psi) * (0,0,-1) = (-sin psi, 0, -cos psi)
        };
        constexpr double kPi = std::numbers::pi;
        const double psiRef = heading(qRef, 0.0);
        const double win = std::max(m_cfg.windowYawSec, 1e-3f);
        const double sig = std::max(win * 0.5, 1e-3);
        double acc = 0.0, wsum = 0.0;
        for (size_t i = lowerBound(m_t, t - win); i < m_t.size(); ++i) {
            const double dtt = m_t[i] - t;
            if (dtt > win)
                break;
            double d = heading(toQ(m_qRaw[i]), psiRef) - psiRef;
            d -= 2.0 * kPi * std::round(d / (2.0 * kPi)); // unwrap around psiRef
            const double w = std::exp(-0.5 * dtt * dtt / (sig * sig));
            acc += w * d;
            wsum += w;
        }
        const double psi = psiRef + (wsum > 0.0 ? acc / wsum : 0.0);
        const Q qLevel{std::cos(psi * 0.5), 0.0, std::sin(psi * 0.5), 0.0};
        qSmooth = amount >= 1.0 ? qLevel : qslerp(qSmooth, qLevel, amount);
    }
    // Fixed mount offset (tilted rig), in the view frame.
    if (m_cfg.pitchOffsetRad != 0.0f || m_cfg.rollOffsetRad != 0.0f)
        qSmooth = qnorm(qmul(qSmooth, qFromYPR(0.0, m_cfg.pitchOffsetRad, m_cfg.rollOffsetRad)));

    // De-rotation prepended to the view ray:  d_body = R(raw)^T R(smooth) d_view.
    const Q qe = qmul(qconj(qRef), qSmooth);
    double md[3][3];
    qToMat(qe, md);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r][c] = static_cast<float>(md[r][c]);
}

// --- generic helpers -------------------------------------------------------

void YawPitchRollToMatrix(float yaw, float pitch, float roll, float m[3][3])
{
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    m[0][0] = cy * cr + sy * sp * sr;
    m[0][1] = -cy * sr + sy * sp * cr;
    m[0][2] = sy * cp;
    m[1][0] = cp * sr;
    m[1][1] = cp * cr;
    m[1][2] = -sp;
    m[2][0] = -sy * cr + cy * sp * sr;
    m[2][1] = sy * sr + cy * sp * cr;
    m[2][2] = cy * cp;
}

void MatrixToYawPitchRoll(const float m[3][3], float& yaw, float& pitch, float& roll)
{
    pitch = std::asin(std::clamp(-m[1][2], -1.0f, 1.0f));
    roll = std::atan2(m[1][0], m[1][1]);
    yaw = std::atan2(m[0][2], m[2][2]);
}

void MatrixMul(const float a[3][3], const float b[3][3], float out[3][3])
{
    float t[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r][c] = t[r][c];
}

} // namespace insv
