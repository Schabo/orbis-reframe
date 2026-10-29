// SPDX-License-Identifier: MIT OR Apache-2.0
#include "Motion.h"

#include "GyroStabilizer.h"
#include "Smooth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace insv {

namespace {

using V3 = std::array<double, 3>;

double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V3 scaled(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
V3 minus(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 normalized(const V3& a)
{
    const double m = std::sqrt(dot(a, a));
    return m > 0.0 ? scaled(a, 1.0 / m) : a;
}
// `a` with its part along unit `n` taken out, normalized.
V3 across(const V3& a, const V3& n) { return normalized(minus(a, scaled(n, dot(a, n)))); }

using M3 = std::array<V3, 3>;
// The body->world matrix of a unit {w,x,y,z}.
M3 matrixOf(const std::array<double, 4>& q)
{
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    return {V3{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
            V3{2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
            V3{2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
}
V3 transposedTimes(const M3& m, const V3& v)
{
    return {m[0][0] * v[0] + m[1][0] * v[1] + m[2][0] * v[2],
            m[0][1] * v[0] + m[1][1] * v[1] + m[2][1] * v[2],
            m[0][2] * v[0] + m[1][2] * v[1] + m[2][2] * v[2]};
}

constexpr double kLeanSmoothSec = 0.25;
constexpr double kAccelSmoothSec = 0.5;
// The turn rate the travel-axis fit follows: smoothed like the lean.
constexpr double kTurnSmoothSec = 0.25;
// Below this the fit is taken as not finding the axis. The X5 samples fit
// at 0.65-0.79 on rides with corners, 0.55 on a 42 s town clip.
constexpr double kMinTravelFit = 0.3;
// The turn rate's spread (rad/s, RMS) under which the clip doesn't turn
// enough to fit anything: a straight ride's wobble.
constexpr double kMinTurnRms = 0.02;

} // namespace

MotionTrack BuildMotion(const ImuTrack& track)
{
    MotionTrack out;
    if (track.samples.size() < 2)
        return out;
    const bool anyAccel =
        std::any_of(track.samples.begin(), track.samples.end(), [](const ImuSample& x) {
            return x.accel[0] != 0.0 || x.accel[1] != 0.0 || x.accel[2] != 0.0;
        });
    if (!anyAccel)
        return out;

    GyroStabilizer level;
    level.build(track, StabConfig::preset(StabMode::HorizonLock));
    if (!level.ready())
        return out;

    std::vector<ImuSample> s = track.samples;
    std::sort(s.begin(), s.end(),
              [](const ImuSample& a, const ImuSample& b) { return a.timestamp < b.timestamp; });
    const AxisMap axes = ParseAxisMap(track.axisMap);
    const double t0 = static_cast<double>(s.front().timestamp);
    const double offset = level.effectiveTimeOffsetSec();
    const double rate = level.clockRatePpm() * 1e-6;
    const auto videoTime = [&](const ImuSample& x) {
        return ((static_cast<double>(x.timestamp) - t0) * 1e-6 - offset) / (1.0 + rate);
    };
    const double step = out.stepSec;
    const double end = videoTime(s.back());
    if (!(end > step))
        return out;
    const size_t n = static_cast<size_t>(end / step) + 1;

    // Each step's mean accelerometer and gyro (sphere axes), and the levelled
    // orientation then.
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    std::vector<V3> up(n), accel(n), gyro(n);
    std::vector<M3> toWorld(n);
    std::vector<double> turnRight(n, kNaN); // rad/s, positive turning right
    std::vector<char> ok(n, 0);
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) * step;
        while (j < s.size() && videoTime(s[j]) < t - step / 2)
            ++j;
        V3 a{0, 0, 0}, g{0, 0, 0};
        int count = 0;
        for (size_t k = j; k < s.size() && videoTime(s[k]) < t + step / 2; ++k) {
            double ma[3], mg[3];
            axes.apply(s[k].accel, ma);
            axes.apply(s[k].gyro, mg);
            for (int c = 0; c < 3; ++c)
                a[c] += ma[c], g[c] += mg[c];
            ++count;
        }
        if (count == 0)
            continue;
        accel[i] = scaled(a, 1.0 / count);
        gyro[i] = scaled(g, 1.0 / count);
        toWorld[i] = matrixOf(level.rawOrientation(t));
        ok[i] = 1;
    }
    // World up in the body: the fusion's level, which it takes only from
    // where the vehicle isn't turning (GyroStabilizer), so a corner doesn't
    // lean it.
    for (size_t i = 0; i < n; ++i) {
        if (!ok[i])
            continue;
        up[i] = normalized(transposedTimes(toWorld[i], {0.0, 1.0, 0.0}));
        // A turn to the right is clockwise seen from above: negative about up.
        turnRight[i] = -dot(gyro[i], up[i]);
    }

    // The mean posture, and the level axes the camera's own forward gives.
    V3 meanUp{0, 0, 0};
    size_t valid = 0;
    for (size_t i = 0; i < n; ++i)
        if (ok[i])
            meanUp = {meanUp[0] + up[i][0], meanUp[1] + up[i][1], meanUp[2] + up[i][2]}, ++valid;
    if (valid < 2)
        return out;
    meanUp = normalized(meanUp);
    V3 forward = across({0.0, 0.0, -1.0}, meanUp);
    if (!(std::fabs(dot(forward, forward) - 1.0) < 1e-6)) // looking straight up or down
        forward = across({1.0, 0.0, 0.0}, meanUp);
    V3 right = cross(forward, meanUp);

    // The travel axis: the level direction in which the tilt (world up in
    // the body) goes with the turn rate, the tilt's covariance with it.
    // Leaning right puts world up toward the body's left, so right is
    // against it. (Not a least-squares fit of the turn rate on the tilt: the
    // lean is all of the tilt that follows the turns, so its two components
    // are near collinear and such a fit takes any direction across them.)
    {
        const std::vector<double> turn = GaussianSmooth(turnRight, step, kTurnSmoothSec);
        double m1 = 0, m2 = 0, mt = 0;
        for (size_t i = 0; i < n; ++i)
            if (ok[i])
                m1 += dot(up[i], forward), m2 += dot(up[i], right), mt += turn[i];
        m1 /= valid, m2 /= valid, mt /= valid;
        double s11 = 0, s12 = 0, s22 = 0, s1t = 0, s2t = 0, stt = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!ok[i])
                continue;
            const double d1 = dot(up[i], forward) - m1, d2 = dot(up[i], right) - m2;
            const double dt = turn[i] - mt;
            s11 += d1 * d1, s12 += d1 * d2, s22 += d2 * d2;
            s1t += d1 * dt, s2t += d2 * dt, stt += dt * dt;
        }
        const double c = std::hypot(s1t, s2t);
        if (c > 0.0 && std::sqrt(stt / valid) >= kMinTurnRms) {
            const double c1 = s1t / c, c2 = s2t / c;
            // The tilt along it against the turn rate: their correlation.
            const double along = c1 * c1 * s11 + 2.0 * c1 * c2 * s12 + c2 * c2 * s22;
            const double fit = along > 0.0 ? (c1 * s1t + c2 * s2t) / std::sqrt(along * stt) : 0.0;
            if (fit >= kMinTravelFit) {
                right = normalized(scaled({c1 * forward[0] + c2 * right[0],
                                           c1 * forward[1] + c2 * right[1],
                                           c1 * forward[2] + c2 * right[2]},
                                          -1.0));
                forward = cross(meanUp, right);
                out.travelFit = fit;
            }
        }
    }

    out.leanDeg.assign(n, std::numeric_limits<float>::quiet_NaN());
    out.lateralG.assign(n, std::numeric_limits<float>::quiet_NaN());
    out.forwardG.assign(n, std::numeric_limits<float>::quiet_NaN());
    constexpr double kDeg = 180.0 / 3.14159265358979323846;
    for (size_t i = 0; i < n; ++i) {
        if (!ok[i])
            continue;
        // At rest the accelerometer reads +1 g up; what's left over is the
        // acceleration. The travel's directions, level at this moment.
        const V3 a = minus(accel[i], up[i]);
        const V3 fwdLevel = across(forward, up[i]);
        const V3 rightLevel = cross(fwdLevel, up[i]);
        out.leanDeg[i] =
            static_cast<float>(std::asin(std::clamp(-dot(up[i], right), -1.0, 1.0)) * kDeg);
        out.lateralG[i] = static_cast<float>(dot(a, rightLevel));
        out.forwardG[i] = static_cast<float>(dot(a, fwdLevel));
    }
    out.leanDeg = GaussianSmooth(out.leanDeg, step, kLeanSmoothSec);
    out.lateralG = GaussianSmooth(out.lateralG, step, kAccelSmoothSec);
    out.forwardG = GaussianSmooth(out.forwardG, step, kAccelSmoothSec);
    return out;
}

} // namespace insv
