// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvImu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace insv {

ImuTrack MakeInsvImuTrack(InsvTelemetry&& tele, double fps, const std::string& axisMap) {
    ImuTrack track;
    track.axisMap = axisMap;
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
