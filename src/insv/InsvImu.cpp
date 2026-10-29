// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvImu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <utility>
#include <vector>

namespace insv {

namespace {

// Row-major rotation matrix of the unit quaternion {w, x, y, z}: v' = M v.
void QuatToMatrix(const double q[4], double m[3][3]) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
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

// World up in the IMU's axes at each attitude sample. The attitude is body
// -> world in north-east-down, so up is -z of the world; body <- IMU is
// Ry(tilt) * R(mount)^T (InsvModel::imuMount). Fitted on the A1's three
// sample clips by matching the attitude's frame-to-frame rotation against
// the gyro: R(field 69)^T is the axis permutation, the tilt 9.5-9.9 deg,
// 0.09-0.14 deg left per 20 ms step.
std::vector<UpSample> UpFromAttitude(const std::vector<AttitudeSample>& attitude,
                                     const double mount[4], double tiltDeg) {
    double m[3][3];
    QuatToMatrix(mount, m);
    const double a = tiltDeg * std::numbers::pi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    const double ry[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
    double bodyFromImu[3][3]; // Ry * M^T
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            bodyFromImu[r][k] = ry[r][0] * m[k][0] + ry[r][1] * m[k][1] + ry[r][2] * m[k][2];
    std::vector<UpSample> up;
    up.reserve(attitude.size());
    for (const AttitudeSample& at : attitude) {
        double wb[3][3];
        QuatToMatrix(at.q, wb);
        // Up in body axes: R^T (0, 0, -1), the third row of R negated.
        const double ub[3] = {-wb[2][0], -wb[2][1], -wb[2][2]};
        UpSample u;
        u.timestamp = at.timestamp;
        for (int k = 0; k < 3; ++k) // IMU <- body is the transpose
            u.up[k] = bodyFromImu[0][k] * ub[0] + bodyFromImu[1][k] * ub[1] +
                      bodyFromImu[2][k] * ub[2];
        up.push_back(u);
    }
    std::sort(up.begin(), up.end(),
              [](const UpSample& x, const UpSample& y) { return x.timestamp < y.timestamp; });
    return up;
}

} // namespace

ImuTrack MakeInsvImuTrack(InsvTelemetry&& tele, double fps, const InsvModel& model) {
    ImuTrack track;
    track.axisMap = model.imuAxes;
    if (!tele.attitude.empty()) {
        if (!tele.has_imu_mount)
            std::fprintf(stderr,
                         "insv: attitude track without field 69; taking the %s's IMU mount "
                         "{%g, %g, %g, %g}\n",
                         model.name, model.imuMount[0], model.imuMount[1], model.imuMount[2],
                         model.imuMount[3]);
        track.up = UpFromAttitude(tele.attitude, tele.has_imu_mount ? tele.imu_mount : model.imuMount,
                                  model.attitudeTiltDeg);
    }
    const std::vector<ExposureSample>& exposure = tele.exposure;
    // The stabilizer sorts the track and puts t = 0 at its earliest sample.
    uint64_t imuT0 = 0;
    if (!tele.imu.empty())
        imuT0 = std::min_element(tele.imu.begin(), tele.imu.end(),
                                 [](const ImuSample& a, const ImuSample& b) {
                                     return a.timestamp < b.timestamp;
                                 })->timestamp;
    // Frame-clock rate: the exposure record has one device timestamp
    // per frame from field 24's frame on; least-squares slope of those vs the
    // nominal n / fps. Linear on every sample clip (~+40 / -15 ppm).
    double clockRate = 0.0;
    if (tele.first_frame_timestamp != 0 && fps > 1.0 && exposure.size() >= 300) {
        size_t k = 0;
        uint64_t best = UINT64_MAX;
        for (size_t i = 0; i < exposure.size(); ++i) {
            const uint64_t ts = exposure[i].timestamp;
            const uint64_t d = ts > tele.first_frame_timestamp ? ts - tele.first_frame_timestamp
                                                               : tele.first_frame_timestamp - ts;
            if (d < best) {
                best = d;
                k = i;
            }
        }
        const size_t n = exposure.size() - k;
        if (n >= 300) {
            const double xm = 0.5 * static_cast<double>(n - 1) / fps;
            double ym = 0.0;
            for (size_t i = 0; i < n; ++i)
                ym += static_cast<double>(exposure[k + i].timestamp - exposure[k].timestamp) * 1e-6;
            ym /= static_cast<double>(n);
            double sxy = 0.0, sxx = 0.0;
            for (size_t i = 0; i < n; ++i) {
                const double x = static_cast<double>(i) / fps - xm;
                const double y =
                    static_cast<double>(exposure[k + i].timestamp - exposure[k].timestamp) * 1e-6 - ym;
                sxy += x * y;
                sxx += x * x;
            }
            const double r = sxx > 0.0 ? sxy / sxx - 1.0 : 0.0;
            if (std::fabs(r) < 500e-6) // sanity: a real crystal is tens of ppm off
                clockRate = r;
        }
    }
    track.clockRate = clockRate;

    // Capture timing. Per-cell image-motion lags on four clip segments (both
    // readout modes, ~570 cells mapped to lens + sensor row) show both lenses
    // read out top to bottom over field 25, in step, with the centre row
    // kCenterRowMs from field 24 + field 28. The earlier per-lens model (lens 1
    // a third of the readout late) was this row gradient seen through views
    // that happened to sit in different rows.
    constexpr double kCenterRowMs = -2.3;
    track.readoutSec = tele.first_frame_timestamp != 0 && tele.rolling_shutter_ms > 0.0
                           ? tele.rolling_shutter_ms * 1e-3
                           : 0.0;

    // Anchor on the first recorded frame (field 24), else estimate from the
    // exposure record.
    if (!tele.imu.empty()) {
        if (tele.first_frame_timestamp != 0) {
            // A trimmed file's video t = 0 is time_map_offset_ms into the
            // recording on the (drifting) frame clock.
            track.hasOffset = true;
            track.offsetSec = (static_cast<double>(tele.first_frame_timestamp) -
                               static_cast<double>(imuT0)) * 1e-6 +
                              tele.gyro_timestamp_ms * 1e-3 +
                              tele.time_map_offset_ms * 1e-3 * (1.0 + clockRate) +
                              kCenterRowMs * 1e-3;
        } else if (exposure.size() >= 32 && fps > 1.0) {
            const double imu0 = static_cast<double>(imuT0) * 1e-6;
            const double frameDt = 1.0 / fps;
            std::vector<double> off;
            off.reserve(exposure.size());
            for (size_t n = 0; n < exposure.size(); ++n)
                off.push_back(static_cast<double>(exposure[n].timestamp) * 1e-6 - imu0 -
                              static_cast<double>(n) * frameDt);
            std::nth_element(off.begin(), off.begin() + off.size() / 2, off.end());
            track.hasOffset = true;
            track.offsetSec = off[off.size() / 2];
        }
    }
    track.samples = std::move(tele.imu);
    return track;
}

} // namespace insv
