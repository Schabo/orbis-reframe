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

// World up and north in the IMU's axes at each attitude sample. The attitude
// is body -> world in north-east-down, so up is -z of the world and north
// +x; body <- IMU is Ry(tilt) * R(mount)^T (InsvModel::imuMount). Fitted on
// the A1's three sample clips by matching the attitude's frame-to-frame
// rotation against the gyro: R(field 69)^T is the axis permutation, the tilt
// 9.5-9.9 deg, 0.09-0.14 deg left per 20 ms step.
std::vector<OrientationSample> OrientationFromAttitude(const std::vector<AttitudeSample>& attitude,
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
    // IMU <- body is the transpose.
    const auto toImu = [&](const double b[3], double out[3]) {
        for (int k = 0; k < 3; ++k)
            out[k] = bodyFromImu[0][k] * b[0] + bodyFromImu[1][k] * b[1] + bodyFromImu[2][k] * b[2];
    };
    std::vector<OrientationSample> out;
    out.reserve(attitude.size());
    for (const AttitudeSample& at : attitude) {
        double wb[3][3];
        QuatToMatrix(at.q, wb);
        // A world direction in body axes is R^T applied to it: up (0, 0, -1)
        // is the third row of R negated, north (1, 0, 0) the first.
        const double ub[3] = {-wb[2][0], -wb[2][1], -wb[2][2]};
        const double nb[3] = {wb[0][0], wb[0][1], wb[0][2]};
        OrientationSample o;
        o.timestamp = at.timestamp;
        toImu(ub, o.up);
        toImu(nb, o.north);
        out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const OrientationSample& x, const OrientationSample& y) {
        return x.timestamp < y.timestamp;
    });
    return out;
}

// A timelapse's orientation record (InsvTelemetry::timelapse_quats, X5
// §3.5) as an orientation-only track: each frame's entries -- a run of them
// across its readout -- matched to the frame's own time (record 0x600) by
// their centre entry, which sits half an exposure before it, and kept with
// that frame's video time; runs that match no frame (the X5 writes one
// stray run minutes before the first frame on some clips) are dropped. The
// quaternion turns world vectors into the camera's axes, so world up (z) and
// north (x) in those axes are its third and first columns.
void TimelapseQuatTrack(const InsvTelemetry& tele, double fps, ImuTrack& track) {
    constexpr uint64_t kRunGapUs = 50000;   // entries closer than this are one frame's
    constexpr uint64_t kMatchUs = 100000;   // centre entry to the frame's time, at most
    std::vector<TimelapseQuat> q = tele.timelapse_quats;
    std::sort(q.begin(), q.end(), [](const TimelapseQuat& a, const TimelapseQuat& b) {
        return a.timestamp < b.timestamp;
    });
    const std::vector<uint64_t>& frames = tele.frame_timestamps;
    struct Run {
        size_t first, last; // entries [first, last]
        size_t frame;
    };
    std::vector<Run> runs;
    for (size_t i = 0; i < q.size();) {
        size_t j = i;
        while (j + 1 < q.size() && q[j + 1].timestamp - q[j].timestamp < kRunGapUs)
            ++j;
        const uint64_t centre = q[(i + j) / 2].timestamp;
        const auto it = std::lower_bound(frames.begin(), frames.end(), centre);
        size_t best = frames.size();
        uint64_t bestD = kMatchUs;
        for (auto c = it == frames.begin() ? it : it - 1; c != frames.end() && c <= it; ++c) {
            const uint64_t d = *c > centre ? *c - centre : centre - *c;
            if (d < bestD) {
                bestD = d;
                best = static_cast<size_t>(c - frames.begin());
            }
        }
        if (best < frames.size() && (runs.empty() || runs.back().frame < best))
            runs.push_back({i, j, best});
        i = j + 1;
    }
    const size_t dropped = [&] {
        size_t kept = 0;
        for (const Run& r : runs)
            kept += r.last - r.first + 1;
        return q.size() - kept;
    }();
    if (dropped > 0)
        std::fprintf(stderr, "insv: %zu timelapse orientation entries match no frame; left out\n",
                     dropped);
    if (runs.size() < 2)
        return;
    const uint64_t t0 = q[runs.front().first].timestamp;
    for (const Run& r : runs) {
        for (size_t i = r.first; i <= r.last; ++i) {
            double m[3][3];
            QuatToMatrix(q[i].q, m);
            OrientationSample o;
            o.timestamp = q[i].timestamp;
            for (int k = 0; k < 3; ++k) {
                o.up[k] = m[k][2];
                o.north[k] = m[k][0];
            }
            track.orientation.push_back(o);
        }
        track.frameTimes.push_back(
            {static_cast<double>(r.frame) / fps - tele.time_map_offset_ms * 1e-3,
             static_cast<double>(q[(r.first + r.last) / 2].timestamp - t0) * 1e-6});
    }
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
        track.orientation = OrientationFromAttitude(
            tele.attitude, tele.has_imu_mount ? tele.imu_mount : model.imuMount,
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
    // Video time to the camera's clock, by the mode (a timelapse, slow
    // motion; 1 otherwise).
    const double scale = CaptureScale(tele, fps);
    track.captureScale = scale;
    if (scale != 1.0)
        std::fprintf(stderr, "insv: %g s of the camera's clock per second of video (%s)\n", scale,
                     scale > 1.0 ? "timelapse" : "slow motion");
    // In a timelapse the exposure record runs at the sensor's rate over the
    // whole time, not one entry per stored frame (its frames' own times,
    // record 0x600, time it instead).
    const bool exposurePerFrame =
        tele.frame_timestamps.empty() && !(tele.timelapse_interval_ms > 0.0);

    // Frame-clock rate: the exposure record has one device timestamp
    // per frame from field 24's frame on; least-squares slope of those vs the
    // nominal n / fps on the camera's clock. Linear on every sample clip
    // (~+40 / -15 ppm).
    double clockRate = 0.0;
    if (exposurePerFrame && tele.first_frame_timestamp != 0 && fps > 1.0 &&
        exposure.size() >= 300) {
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
            const double r = sxx > 0.0 ? sxy / sxx / scale - 1.0 : 0.0;
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
                              tele.time_map_offset_ms * 1e-3 * scale * (1.0 + clockRate) +
                              kCenterRowMs * 1e-3;
        } else if (exposurePerFrame && exposure.size() >= 32 && fps > 1.0) {
            const double imu0 = static_cast<double>(imuT0) * 1e-6;
            const double frameDt = scale / fps;
            std::vector<double> off;
            off.reserve(exposure.size());
            for (size_t n = 0; n < exposure.size(); ++n)
                off.push_back(static_cast<double>(exposure[n].timestamp) * 1e-6 - imu0 -
                              static_cast<double>(n) * frameDt);
            std::nth_element(off.begin(), off.begin() + off.size() / 2, off.end());
            track.hasOffset = true;
            track.offsetSec = off[off.size() / 2];
        }
        // A timelapse's frames by their own times (record 0x600), which
        // needn't be evenly spaced, through the same terms as the offset.
        if (tele.first_frame_timestamp != 0 && fps > 1.0 && tele.frame_timestamps.size() >= 2)
            for (size_t n = 0; n < tele.frame_timestamps.size(); ++n)
                track.frameTimes.push_back(
                    {static_cast<double>(n) / fps - tele.time_map_offset_ms * 1e-3,
                     (static_cast<double>(tele.frame_timestamps[n]) - static_cast<double>(imuT0)) *
                             1e-6 +
                         tele.gyro_timestamp_ms * 1e-3 + kCenterRowMs * 1e-3});
    } else if (!tele.timelapse_quats.empty()) {
        // No gyro, but the orientation a timelapse stores in its place.
        if (!model.timelapseQuatAxes)
            std::fprintf(stderr, "insv: a timelapse orientation record, but its axes aren't known "
                                 "for the %s; not stabilized\n",
                         model.name);
        else if (tele.frame_timestamps.empty() || !(fps > 1.0))
            std::fprintf(stderr, "insv: a timelapse orientation record without the frames' own "
                                 "times; not stabilized\n");
        else {
            track.axisMap = model.timelapseQuatAxes;
            TimelapseQuatTrack(tele, fps, track);
            if (!track.frameTimes.empty()) {
                const FrameTime& f = track.frameTimes.front();
                track.hasOffset = true;
                track.offsetSec = f.trackSec - f.videoSec * scale;
            }
        }
    }
    track.samples = std::move(tele.imu);
    return track;
}

} // namespace insv
