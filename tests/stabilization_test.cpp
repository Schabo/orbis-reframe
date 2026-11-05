// Stabilization: insv::StabConfig and the gyro stabilizer on synthetic IMU
// data (lean follow, horizon lock, direction lock, auto-level, readout), and
// the per-clip setting on the Timeline and in the project file.
// Standalone (no event loop: same-thread signals fire inside emit); the
// exit code is the failure count.

#include "support/ModelTest.h"

#include <InsvImu.h>    // insv::MakeInsvImuTrack
#include <InsvModels.h> // insv::FindInsvModel

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace {

// The synthetic IMU samples below are in the X5's sensor axes.
const char *x5Axes() { return insv::FindInsvModel("Insta360 X5").imuAxes; }
insv::StabConfig x5(insv::StabConfig c)
{
    c.gyroAxes = x5Axes();
    return c;
}

void testStabConfig()
{
    // Lean-follow Strength (0..1) <-> shared smoothing window (s), the mapping
    // the Stabilization panel's slider rides on.
    CHECK(insv::leanWindowFromStrength(0.0f) < insv::leanWindowFromStrength(0.5f));
    CHECK(insv::leanWindowFromStrength(0.5f) < insv::leanWindowFromStrength(1.0f));
    // 0.5 reproduces the historical ~0.9 s LeanFollow default.
    CHECK(std::fabs(insv::leanWindowFromStrength(0.5f) - 0.9f) < 0.05f);
    CHECK(insv::StabConfig::preset(insv::StabMode::LeanFollow).windowRollSec ==
          insv::leanWindowFromStrength(0.5f));
    // mutually inverse in the interior
    for (float s : {0.1f, 0.35f, 0.6f, 0.9f}) {
        const float back = insv::leanStrengthFromWindow(insv::leanWindowFromStrength(s));
        CHECK(std::fabs(back - s) < 1e-3f);
    }
    // clamp-safe both directions
    CHECK(insv::leanWindowFromStrength(-1.0f) == insv::leanWindowFromStrength(0.0f));
    CHECK(insv::leanWindowFromStrength(2.0f) == insv::leanWindowFromStrength(1.0f));
    CHECK(insv::leanStrengthFromWindow(0.01f) == 0.0f);
    CHECK(insv::leanStrengthFromWindow(99.0f) == 1.0f);
    CHECK(std::fabs(insv::leanWindowFromStrength(1.0f) - 5.0f) < 1e-4f); // heavy-smoothing end

    // Horizon-lock Smoothing <-> heading window: 0.3..10 s, 0.5 == the preset.
    CHECK(std::fabs(insv::horizonWindowFromStrength(0.0f) - 0.3f) < 1e-4f);
    CHECK(std::fabs(insv::horizonWindowFromStrength(1.0f) - 10.0f) < 1e-3f);
    CHECK(std::fabs(insv::horizonWindowFromStrength(0.5f) -
                    insv::StabConfig::preset(insv::StabMode::HorizonLock).windowYawSec) < 1e-3f);
    for (float s : {0.0f, 0.2f, 0.5f, 0.8f, 1.0f})
        CHECK(std::fabs(insv::horizonStrengthFromWindow(insv::horizonWindowFromStrength(s)) - s) <
              1e-3f);
    CHECK(insv::horizonWindowFromStrength(0.3f) < insv::horizonWindowFromStrength(0.7f));

    // MakeInsvImuTrack fits the frame clock's rate against the nominal fps
    // from the exposure record. The offset targets the centre sensor
    // row (-2.3 ms), and the readout (field 25) is reported for the stitch.
    // Synthetic: 1 kHz IMU from device t = 1 s, frames on a +40 ppm clock with
    // 5 pre-roll exposures before field 24's frame, 20 ms readout.
    {
        const double fps = 30000.0 / 1001.0;
        insv::InsvTelemetry tele;
        for (int i = 0; i < 700000; ++i) { // ~700 s
            insv::ImuSample s;
            s.timestamp = 1000000 + static_cast<uint64_t>(i) * 1000; // us
            s.accel[1] = 1.0;
            tele.imu.push_back(s);
        }
        const double rate = 40e-6;
        const uint64_t f24 = 1500000; // 0.5 s after IMU[0]
        for (int n = -5; n < 20000; ++n) {
            insv::ExposureSample e;
            e.timestamp = static_cast<uint64_t>(
                static_cast<int64_t>(f24) + std::llround(n / fps * (1.0 + rate) * 1e6));
            tele.exposure.push_back(e);
        }
        tele.first_frame_timestamp = f24;
        tele.gyro_timestamp_ms = 1.6;
        tele.rolling_shutter_ms = 20.0;

        const insv::ImuTrack track =
            insv::MakeInsvImuTrack(insv::InsvTelemetry(tele), fps,
                                   insv::FindInsvModel("Insta360 X5"));
        CHECK(track.axisMap == x5Axes());
        insv::GyroStabilizer g;
        g.build(track, insv::StabConfig::preset(insv::StabMode::LeanFollow));
        CHECK(g.ready());
        CHECK(std::fabs(g.clockRatePpm() - 40.0) < 0.5);
        // 0.5 s pre-roll + 1.6 ms IMU latency - 2.3 ms to the centre sensor row
        CHECK(std::fabs(g.effectiveTimeOffsetSec() - 0.4993f) < 1e-4f);
        CHECK(std::fabs(g.readoutSec() - 0.020) < 1e-12);
        // A measured fine-tune adds to the derived offset.
        g.build(track, insv::StabConfig::preset(insv::StabMode::LeanFollow), 0.003);
        CHECK(std::fabs(g.effectiveTimeOffsetSec() - 0.5023f) < 1e-4f);

        // Explicit override: offset kept as-is, clock rate still fitted.
        insv::StabConfig ov = insv::StabConfig::preset(insv::StabMode::LeanFollow);
        ov.timeOffsetSec = 0.25f;
        g.build(track, ov);
        CHECK(g.effectiveTimeOffsetSec() == 0.25f);
        CHECK(std::fabs(g.clockRatePpm() - 40.0) < 0.5);

        // The plain IMU overload knows nothing about frames: nominal time.
        g.build(track.samples, x5(insv::StabConfig::preset(insv::StabMode::LeanFollow)));
        CHECK(g.clockRatePpm() == 0.0);
    }

    // Slow motion and timelapse: the camera's clock runs captureScale times the
    // video's. A steady turn of w rad/s about raw gyro z, 40 s of IMU; the
    // video turns w * captureScale rad per second of it.
    {
        auto turned = [](const insv::GyroStabilizer& g, double t0, double t1) {
            const auto a = g.rawOrientation(t0), b = g.rawOrientation(t1);
            const double dot = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
            return 2.0 * std::acos(std::min(1.0, dot));
        };
        auto telemetry = [](double w) {
            insv::InsvTelemetry tele;
            for (int i = 0; i < 40000; ++i) {
                insv::ImuSample s;
                s.timestamp = 1000000 + static_cast<uint64_t>(i) * 1000; // us, 1 kHz
                s.gyro[2] = w;
                tele.imu.push_back(s);
            }
            tele.first_frame_timestamp = 1500000;
            return tele;
        };
        const insv::InsvModel& a1 = insv::FindInsvModel("Antigravity A1");

        // 4K Slow Motion: 100 fps stored at 29.97, one exposure entry per
        // stored frame, 10 ms apart, on a +40 ppm clock.
        insv::InsvTelemetry slow = telemetry(0.5);
        slow.capture_fps = 100.0;
        slow.stored_fps = 29.97;
        for (int n = 0; n < 3000; ++n) {
            insv::ExposureSample e;
            e.timestamp = 1500000 + static_cast<uint64_t>(std::llround(n * 10000.0 * (1.0 + 40e-6)));
            slow.exposure.push_back(e);
        }
        const insv::ImuTrack st = insv::MakeInsvImuTrack(std::move(slow), 29.97, a1);
        CHECK(std::fabs(st.captureScale - 0.2997) < 1e-12);
        CHECK(std::fabs(st.clockRate - 40e-6) < 1e-6); // fitted against the scale, kept
        insv::GyroStabilizer g;
        g.build(st, insv::StabConfig::preset(insv::StabMode::LeanFollow));
        CHECK(g.ready() && std::fabs(g.captureScale() - 0.2997) < 1e-12);
        CHECK(std::fabs(turned(g, 2.0, 12.0) - 0.5 * 10.0 * 0.2997 * (1.0 + 40e-6)) < 1e-3);
        for (double t : {0.0, 3.0, 50.0})
            CHECK(std::fabs(g.videoTime(g.effectiveTimeOffsetSec() +
                                        t * 0.2997 * (1.0 + g.clockRatePpm() * 1e-6)) -
                            t) < 1e-6);
        // bodyRotation's span is the camera's clock (a sensor readout).
        float r[3];
        g.bodyRotation(3.0, 0.01, r);
        CHECK(std::fabs(std::hypot(r[0], r[1], r[2]) - 0.005f) < 1e-5f);

        // Timelapse, a frame every 2 s at 29.97 by record 0x600; the exposure
        // record runs at the sensor rate and isn't fitted.
        insv::InsvTelemetry lapse = telemetry(0.01);
        lapse.timelapse_interval_ms = 2000.0;
        for (int n = 0; n < 19; ++n)
            lapse.frame_timestamps.push_back(1500000 + static_cast<uint64_t>(n) * 2002000);
        for (int n = 0; n < 1000; ++n) {
            insv::ExposureSample e;
            e.timestamp = 1500000 + static_cast<uint64_t>(std::llround(n * 33366.7));
            lapse.exposure.push_back(e);
        }
        const insv::ImuTrack lt = insv::MakeInsvImuTrack(std::move(lapse), 30000.0 / 1001.0, a1);
        CHECK(std::fabs(lt.captureScale - 2.002 * 30000.0 / 1001.0) < 1e-9);
        CHECK(lt.clockRate == 0.0);
        // A stabilizer of its own: its track cache knows a track by its
        // samples' count and ends, which this one shares with the one above.
        insv::GyroStabilizer gl;
        gl.build(lt, insv::StabConfig::preset(insv::StabMode::LeanFollow));
        CHECK(gl.ready());
        CHECK(std::fabs(turned(gl, 0.1, 0.5) - 0.01 * 0.4 * lt.captureScale) < 1e-3);
    }

    // bodyRotation(): the camera turn in the axis-mapped body frame, for the
    // stitch RS correction. A constant 1 rad/s about raw gyro z lands on body X
    // under the X5's "z,-x,-y" map (no accel -> pure gyro integration).
    {
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 3000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000; // us, 1 kHz
            s.gyro[2] = 1.0;
            imu.push_back(s);
        }
        insv::GyroStabilizer g;
        g.build(imu, x5(insv::StabConfig::preset(insv::StabMode::LeanFollow)));
        CHECK(g.ready());
        float r[3];
        g.bodyRotation(1.5, 0.01, r); // 10 ms at 1 rad/s
        CHECK(std::fabs(r[0] - 0.01f) < 1e-5f);
        CHECK(std::fabs(r[1]) < 1e-5f && std::fabs(r[2]) < 1e-5f);
        g.bodyRotation(1.5, -0.01, r);
        CHECK(std::fabs(r[0] + 0.01f) < 1e-5f);
    }

    // 2026-09-25: LeanFollow is pure gyro -- accelerometer junk (a 0.4 g 11 Hz
    // stick resonance) must not move its correction at all. Auto-level (on by
    // default) only reads the clip's mean accel, a constant ~identity here;
    // with it off the correction is exactly identity.
    {
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 3000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000; // us, 1 kHz
            const double n = 2.0 * std::numbers::pi * 11.0 * i * 1e-3;
            s.accel[0] = -1.0;
            s.accel[1] = 0.4 * std::sin(n);
            s.accel[2] = 0.4 * std::sin(n + 1.0);
            imu.push_back(s);
        }
        for (const bool autoLevel : {true, false}) {
            insv::StabConfig lc = insv::StabConfig::preset(insv::StabMode::LeanFollow);
            lc.autoLevel = autoLevel;
            insv::GyroStabilizer g;
            g.build(imu, x5(lc));
            CHECK(g.ready());
            float m0[3][3];
            g.correctionMatrix(0.8, m0);
            float worst = 0.0f, offIdentity = 0.0f;
            for (int k = 0; k < 30; ++k) {
                float m[3][3];
                g.correctionMatrix(0.8 + k / 30.0, m);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) {
                        worst = std::max(worst, std::fabs(m[r][c] - m0[r][c]));
                        offIdentity = std::max(offIdentity,
                                               std::fabs(m[r][c] - (r == c ? 1.0f : 0.0f)));
                    }
            }
            CHECK(worst < 1e-5f);
            CHECK(offIdentity < (autoLevel ? 1e-3f : 1e-5f));
        }
    }

    // Horizon lock levels a tilted mount: gravity-up is 20 deg off body +Y
    // (a rolled camera), gyro still. The correction must carry the view's up
    // onto that measured up, and the view's forward must be horizontal.
    {
        const double a = 20.0 * std::numbers::pi / 180.0;
        const double b[3] = {std::sin(a), std::cos(a), 0.0}; // body up
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 3000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            // the X5's map "z,-x,-y": body X <- z, Y <- -x, Z <- -y
            s.accel[2] = b[0];
            s.accel[0] = -b[1];
            s.accel[1] = -b[2];
            imu.push_back(s);
        }
        insv::GyroStabilizer g;
        g.build(imu, x5(insv::StabConfig::preset(insv::StabMode::HorizonLock)));
        CHECK(g.ready());
        float m[3][3];
        g.correctionMatrix(1.5, m);
        const float up[3] = {m[0][1], m[1][1], m[2][1]};     // C * (0,1,0)
        const float fwd[3] = {-m[0][2], -m[1][2], -m[2][2]}; // C * (0,0,-1)
        CHECK(std::fabs(up[0] - b[0]) < 1e-3f && std::fabs(up[1] - b[1]) < 1e-3f &&
              std::fabs(up[2] - b[2]) < 1e-3f);
        CHECK(std::fabs(fwd[0] * b[0] + fwd[1] * b[1] + fwd[2] * b[2]) < 1e-3f);
    }

    // 2026-10-09: a residual gyro bias (the X5 keeps ~0.3-0.4 deg/s) against
    // the weak accel anchor used to leave Horizon lock several degrees off
    // level. Still camera, gravity 20 deg off body +Y, 0.006 rad/s of bias on
    // the two axes across gravity: the whole-clip fit must level it.
    {
        const double a = 20.0 * std::numbers::pi / 180.0;
        const double b[3] = {std::sin(a), std::cos(a), 0.0}; // body up
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 20000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            s.accel[2] = b[0]; // the X5's map "z,-x,-y"
            s.accel[0] = -b[1];
            s.accel[1] = -b[2];
            s.gyro[1] = -0.006; // body Z (forward axis): rolls the horizon
            s.gyro[2] = 0.004;  // body X: pitches it
            imu.push_back(s);
        }
        insv::GyroStabilizer g;
        g.build(imu, x5(insv::StabConfig::preset(insv::StabMode::HorizonLock)));
        CHECK(g.ready());
        double worst = 0.0;
        for (double t : {0.5, 5.0, 10.0, 19.5}) {
            float m[3][3];
            g.correctionMatrix(t, m);
            const double dot = m[0][1] * b[0] + m[1][1] * b[1] + m[2][1] * b[2];
            worst = std::max(worst, std::acos(std::min(1.0, dot)) * 180.0 / std::numbers::pi);
        }
        CHECK(worst < 0.2);
    }

    // Lean follow auto-level: a still camera on a mount tilted 20 deg. On, the
    // view's up is the mount's gravity up; off, the camera body's own up.
    {
        const double a = 20.0 * std::numbers::pi / 180.0;
        const double b[3] = {std::sin(a), std::cos(a), 0.0}; // body up
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 3000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            s.accel[2] = b[0];
            s.accel[0] = -b[1];
            s.accel[1] = -b[2];
            imu.push_back(s);
        }
        insv::StabConfig lc = insv::StabConfig::preset(insv::StabMode::LeanFollow);
        CHECK(lc.autoLevel);
        insv::GyroStabilizer g;
        g.build(imu, x5(lc));
        float m[3][3];
        g.correctionMatrix(1.5, m);
        CHECK(std::fabs(m[0][1] - b[0]) < 1e-3f && std::fabs(m[1][1] - b[1]) < 1e-3f &&
              std::fabs(m[2][1] - b[2]) < 1e-3f);
        lc.autoLevel = false;
        g.build(imu, x5(lc));
        g.correctionMatrix(1.5, m);
        CHECK(std::fabs(m[0][1]) < 1e-5f && std::fabs(m[1][1] - 1.0f) < 1e-5f);
    }

    // A drone's flight controller knows up better than its accelerometer,
    // which reads the drone's own acceleration too (ImuTrack::orientation). Still
    // camera, true up 20 deg off body +Y, the accel a further 15 deg off
    // that at 1.05 g (the drone speeding up), residual gyro bias: Horizon
    // lock and Lean follow's auto-level take the reference's up; without it
    // they take the accel's, as before.
    {
        const double d2r = std::numbers::pi / 180.0;
        const double a = 20.0 * d2r, p = 15.0 * d2r;
        const double bt[3] = {std::sin(a), std::cos(a), 0.0};                       // true up
        const double ba[3] = {bt[0], bt[1] * std::cos(p), bt[1] * std::sin(p)};    // the accel's
        auto toX5 = [](const double b[3], double out[3]) { // the X5's map "z,-x,-y"
            out[2] = b[0];
            out[0] = -b[1];
            out[1] = -b[2];
        };
        insv::ImuTrack track;
        track.axisMap = x5Axes();
        for (int i = 0; i < 20000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            toX5(ba, s.accel);
            for (double &c : s.accel)
                c *= 1.05;
            s.gyro[1] = -0.006;
            s.gyro[2] = 0.004;
            track.samples.push_back(s);
            if (i % 20 == 0) { // 50 Hz
                insv::OrientationSample u;
                u.timestamp = s.timestamp;
                toX5(bt, u.up);
                track.orientation.push_back(u);
            }
        }
        insv::ImuTrack accelOnly = track;
        accelOnly.orientation.clear();
        // Worst angle between the view's up and `b` over the clip.
        auto worstDeg = [](const insv::GyroStabilizer &g, const double b[3]) {
            double worst = 0.0;
            for (double t : {0.5, 5.0, 10.0, 19.5}) {
                float m[3][3];
                g.correctionMatrix(t, m);
                const double dot = m[0][1] * b[0] + m[1][1] * b[1] + m[2][1] * b[2];
                worst = std::max(worst, std::acos(std::min(1.0, dot)) * 180.0 / std::numbers::pi);
            }
            return worst;
        };
        for (const insv::StabMode mode : {insv::StabMode::HorizonLock, insv::StabMode::LeanFollow}) {
            const insv::StabConfig c = insv::StabConfig::preset(mode);
            insv::GyroStabilizer g;
            g.build(track, c);
            CHECK(g.ready() && worstDeg(g, bt) < 0.2);
            // The same stabilizer again, without the reference: its cached
            // track mustn't be reused.
            g.build(accelOnly, c);
            CHECK(worstDeg(g, ba) < 0.2 && worstDeg(g, bt) > 10.0);
            g.build(track, c);
            CHECK(worstDeg(g, bt) < 0.2);
        }
    }

    // Field 69 missing: the model row's mount stands in for it.
    {
        insv::InsvTelemetry tele;
        for (int i = 0; i < 4; ++i) {
            insv::ImuSample s;
            s.timestamp = 1000000 + static_cast<uint64_t>(i) * 1000;
            tele.imu.push_back(s);
            insv::AttitudeSample at;
            at.timestamp = s.timestamp;
            at.q[0] = std::cos(0.1 * i), at.q[2] = std::sin(0.1 * i);
            tele.attitude.push_back(at);
        }
        const insv::InsvModel &a1 = insv::FindInsvModel("Antigravity A1");
        insv::InsvTelemetry withMount = tele;
        withMount.has_imu_mount = true;
        for (int k = 0; k < 4; ++k)
            withMount.imu_mount[k] = a1.imuMount[k];
        const insv::ImuTrack from69 = insv::MakeInsvImuTrack(std::move(withMount), 30.0, a1);
        const insv::ImuTrack fallback = insv::MakeInsvImuTrack(insv::InsvTelemetry(tele), 30.0, a1);
        CHECK(from69.orientation.size() == 4 && fallback.orientation.size() == 4);
        for (size_t i = 0; i < std::min(from69.orientation.size(), fallback.orientation.size()); ++i)
            for (int k = 0; k < 3; ++k)
                CHECK(from69.orientation[i].up[k] == fallback.orientation[i].up[k] &&
                      from69.orientation[i].north[k] == fallback.orientation[i].north[k]);
        // The cameras record no attitude.
        tele.attitude.clear();
        CHECK(insv::MakeInsvImuTrack(std::move(tele), 30.0, insv::FindInsvModel("Insta360 X5"))
                  .orientation.empty());
    }

    // Direction lock: a level camera still for 2 s, turning 90 deg left over
    // 3 s, still again. Horizon lock's view turns with it; Direction lock's
    // keeps the heading the camera had at directionSec, level, throughout.
    {
        constexpr double kPi = std::numbers::pi;
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 8000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            s.accel[0] = -1.0; // the X5's map "z,-x,-y": body up (+Y) <- -x
            const double t = i / 1000.0;
            if (t >= 2.0 && t < 5.0)
                s.gyro[0] = -(kPi / 2.0) / 3.0; // body yaw (+Y) <- -x
            imu.push_back(s);
        }
        // The view's forward in the world, as a heading (deg), and its tilt.
        auto view = [](const insv::GyroStabilizer &g, double t, double *tiltDeg = nullptr) {
            const std::array<double, 4> q = g.rawOrientation(t);
            const double w = q[0], x = q[1], y = q[2], z = q[3];
            const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                                    {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                                    {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
            float m[3][3];
            g.correctionMatrix(t, m);
            double f[3] = {0, 0, 0}, u[3] = {0, 0, 0};
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) {
                    f[r] -= R[r][k] * m[k][2]; // R * C * (0,0,-1)
                    u[r] += R[r][k] * m[k][1]; // R * C * (0,1,0)
                }
            if (tiltDeg)
                *tiltDeg = std::acos(std::min(1.0, u[1])) * 180.0 / kPi;
            return std::atan2(-f[0], -f[2]) * 180.0 / kPi;
        };
        insv::GyroStabilizer hl;
        hl.build(imu, x5(insv::StabConfig::preset(insv::StabMode::HorizonLock)));
        CHECK(std::fabs(std::fabs(view(hl, 7.0) - view(hl, 1.0)) - 90.0) < 1.0);

        insv::StabConfig dc = insv::StabConfig::preset(insv::StabMode::DirectionLock);
        CHECK(dc.lockDirection && dc.horizonLockAmount == 1.0f);
        dc.directionSec = 1.0f;
        insv::GyroStabilizer dl;
        dl.build(imu, x5(dc));
        CHECK(dl.ready());
        const double held = view(dl, 1.0);
        CHECK(std::fabs(held - view(hl, 1.0)) < 0.5); // no jump where it's locked
        double worst = 0.0, tilt = 0.0, worstTilt = 0.0;
        for (double t = 0.5; t < 7.6; t += 0.25) {
            worst = std::max(worst, std::fabs(view(dl, t, &tilt) - held));
            worstTilt = std::max(worstTilt, tilt);
        }
        CHECK(worst < 0.5);
        CHECK(worstTilt < 0.5);
        // Locked at the end instead: the heading the turn ended on, all along.
        dc.directionSec = 7.0f;
        dl.build(imu, x5(dc));
        CHECK(std::fabs(view(dl, 1.0) - view(hl, 7.0)) < 0.5);
    }

    // The gyro's bias about up, which the bias fit can't see, turns Direction
    // lock's view slowly; a drone's flight controller knows its heading
    // without drift (ImuTrack::orientation's north), and holds it. The same
    // turn over 30 s with a 0.3 deg/s bias about up; the true north in the
    // IMU's axes turns with the camera.
    {
        constexpr double kPi = std::numbers::pi;
        constexpr double kBias = 0.3 * kPi / 180.0;
        insv::ImuTrack track;
        track.axisMap = x5Axes();
        for (int i = 0; i < 30000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            s.accel[0] = -1.0;
            const double t = i / 1000.0;
            s.gyro[0] = -kBias - (t >= 2.0 && t < 5.0 ? (kPi / 2.0) / 3.0 : 0.0);
            track.samples.push_back(s);
            if (i % 20 == 0) { // 50 Hz
                // The camera has turned by th: north, its forward at the
                // start, is (sin th, 0, -cos th) in the body, raw (0, cos th,
                // sin th) through "z,-x,-y".
                const double th = std::clamp((t - 2.0) * kPi / 6.0, 0.0, kPi / 2.0);
                insv::OrientationSample o;
                o.timestamp = s.timestamp;
                o.up[0] = -1.0;
                o.north[1] = std::cos(th);
                o.north[2] = std::sin(th);
                track.orientation.push_back(o);
            }
        }
        // The view's heading in the true world: the camera body has turned
        // by th about +Y, so the view's forward is Ry(th) * C * (0, 0, -1).
        auto heading = [&](const insv::GyroStabilizer &g, double t) {
            const double th = std::clamp((t - 2.0) * kPi / 6.0, 0.0, kPi / 2.0);
            float m[3][3];
            g.correctionMatrix(t, m);
            const double bx = -m[0][2], bz = -m[2][2];
            const double fx = std::cos(th) * bx + std::sin(th) * bz;
            const double fz = -std::sin(th) * bx + std::cos(th) * bz;
            return std::atan2(-fx, -fz) * 180.0 / kPi;
        };
        auto worstTurn = [&](const insv::ImuTrack &tr) {
            insv::StabConfig dc = insv::StabConfig::preset(insv::StabMode::DirectionLock);
            dc.directionSec = 1.0f;
            insv::GyroStabilizer dl;
            dl.build(tr, x5(dc));
            double worst = 0.0;
            for (double t = 0.5; t < 29.5; t += 0.5) {
                double d = heading(dl, t) - heading(dl, 1.0);
                d -= 360.0 * std::round(d / 360.0);
                worst = std::max(worst, std::fabs(d));
            }
            return worst;
        };
        CHECK(worstTurn(track) < 0.1);
        insv::ImuTrack upOnly = track; // a source that knows only up: the gyro's drift
        for (insv::OrientationSample &o : upOnly.orientation)
            o.north[1] = o.north[2] = 0.0;
        CHECK(worstTurn(upOnly) > 5.0);
    }

    // A LeanFollow config with a retuned (non-default) window round-trips
    // through the .orp like any other StabConfig.
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString projFile = tmp.path() + "/lean.orp";
    insv::StabConfig lean = insv::StabConfig::preset(insv::StabMode::LeanFollow);
    lean.windowYawSec = lean.windowPitchSec = lean.windowRollSec =
        insv::leanWindowFromStrength(0.8f);
    {
        Project p;
        const int id = p.timeline()->appendClip("a.insv", 1000, 30.0);
        p.timeline()->setClipStabilization(id, lean);
        CHECK(p.timeline()->clipAt(0).stabilization == lean);
        QString err;
        CHECK(p.save(projFile, &err));
    }
    {
        Project p;
        QString err;
        CHECK(p.load(projFile, &err));
        CHECK(p.timeline()->count() == 1);
        const insv::StabConfig &st = p.timeline()->clipAt(0).stabilization;
        CHECK(st.mode == insv::StabMode::LeanFollow);
        CHECK(std::fabs(st.windowRollSec - insv::leanWindowFromStrength(0.8f)) < 1e-4f);
        // the slider position the panel would show on load
        CHECK(std::fabs(insv::leanStrengthFromWindow(st.windowRollSec) - 0.8f) < 1e-3f);
    }

    // Direction lock round-trips with its direction, as version 15; a
    // project without it keeps the older version, so older builds open it.
    auto versionOf = [](const QString &file) {
        QFile f(file);
        return f.open(QIODevice::ReadOnly)
                   ? QJsonDocument::fromJson(f.readAll()).object().value("version").toInt()
                   : -1;
    };
    CHECK(versionOf(projFile) < 15);
    insv::StabConfig dir = insv::StabConfig::preset(insv::StabMode::DirectionLock);
    dir.directionSec = 12.5f;
    dir.horizonLockAmount = 0.6f;
    const QString dirFile = tmp.path() + "/direction.orp";
    {
        Project p;
        const int id = p.timeline()->appendClip("a.insv", 20000, 30.0);
        p.timeline()->setClipStabilization(id, dir);
        QString err;
        CHECK(p.save(dirFile, &err));
    }
    CHECK(versionOf(dirFile) == 15);
    {
        Project p;
        QString err;
        CHECK(p.load(dirFile, &err));
        CHECK(p.timeline()->count() == 1 && p.timeline()->clipAt(0).stabilization == dir);
    }
}

// A timelapse that records its orientation instead of the gyro (the X5's
// record 0x1800): the truth, how it is recorded, and the same motion as a
// gyro would have recorded it.
namespace lapse {
using M3 = std::array<std::array<double, 3>, 3>;
M3 mul(const M3 &a, const M3 &b)
{
    M3 c{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                c[i][j] += a[i][k] * b[k][j];
    return c;
}
M3 transposed(const M3 &a)
{
    M3 t{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            t[i][j] = a[j][i];
    return t;
}
M3 rotX(double a) { return {{{1, 0, 0}, {0, std::cos(a), -std::sin(a)}, {0, std::sin(a), std::cos(a)}}}; }
M3 rotY(double a) { return {{{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}}}; }
M3 rotZ(double a) { return {{{std::cos(a), -std::sin(a), 0}, {std::sin(a), std::cos(a), 0}, {0, 0, 1}}}; }
M3 fromQuat(const std::array<double, 4> &q)
{
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
             {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
             {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
}
std::array<double, 4> quatOf(const M3 &m) // {w, x, y, z}, Shepperd
{
    const double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0) {
        const double s = 2.0 * std::sqrt(1.0 + tr);
        return {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    }
    if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
        return {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    }
    if (m[1][1] > m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
        return {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
    }
    const double s = 2.0 * std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
    return {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
}
// The angle between two rotations, radians.
double angle(const M3 &a, const M3 &b)
{
    double tr = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            tr += a[i][j] * b[i][j];
    return std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0));
}
M3 fromFloat(const float m[3][3])
{
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i][j] = m[i][j];
    return r;
}

// Camera body -> world in the sphere's axes (+Y up, north at -Z), `t`
// seconds of real time in: a steady turn, a roll sway, a tilted mount.
M3 truth(double t)
{
    return mul(mul(rotY(0.02 * t), rotZ(0.1 * std::sin(2.0 * 3.141592653589793 * t / 30.0))),
               rotX(0.06));
}
constexpr uint64_t kStartUs = 1000000000; // the device clock at the first frame
constexpr double kExposureSec = 0.001;
constexpr double kStepSec = 0.0021244; // between a frame's 11 entries
// Frame n's own time, seconds after the first: 2.0 or 2.1 s apart, as the X5's.
double frameTime(int n)
{
    double t = 0.0;
    for (int i = 0; i < n; ++i)
        t += i % 4 == 1 ? 2.1 : 2.0;
    return t;
}
// `frames` frames' worth of record 0x1800 and 0x600, one run more than
// frames in 0x600 as the X5 writes, plus a stray run 300 s before the first.
insv::InsvTelemetry recorded(int frames)
{
    // The record's axes: world x north, z up; body "y,z,x" to the sphere's.
    const M3 toQWorld = {{{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    const M3 toQBody = {{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}};
    insv::InsvTelemetry tele;
    tele.first_frame_timestamp = kStartUs;
    tele.rolling_shutter_ms = 21.244;
    tele.timelapse_interval_ms = 2000.0;
    for (int n = 0; n <= frames; ++n)
        tele.frame_timestamps.push_back(kStartUs + uint64_t(std::llround(frameTime(n) * 1e6)));
    auto run = [&](double centre) {
        for (int i = -5; i <= 5; ++i) {
            const double t = centre + i * kStepSec;
            insv::TimelapseQuat e;
            e.timestamp = kStartUs + uint64_t(std::llround(t * 1e6));
            const auto q = quatOf(mul(mul(toQBody, transposed(truth(t))), transposed(toQWorld)));
            std::copy(q.begin(), q.end(), e.q);
            tele.timelapse_quats.push_back(e);
        }
    };
    run(-300.0);
    for (int n = 0; n < frames; ++n)
        run(frameTime(n) - kExposureSec / 2);
    return tele;
}
// The same motion as the X5's gyro and accelerometer record it, 1 kHz.
insv::InsvTelemetry gyro(int frames)
{
    insv::InsvTelemetry tele = recorded(frames);
    tele.timelapse_quats.clear();
    const double h = 1e-4;
    for (double t = -1.0; t < frameTime(frames) + 1.0; t += 0.001) {
        const M3 b = truth(t);
        const M3 d = mul(transposed(truth(t - h)), truth(t + h));
        const double w[3] = {(d[2][1] - d[1][2]) / (4 * h), (d[0][2] - d[2][0]) / (4 * h),
                             (d[1][0] - d[0][1]) / (4 * h)};
        const double up[3] = {b[1][0], b[1][1], b[1][2]};
        // Sphere -> the X5's sensor ("z,-x,-y"): raw x = -Y, y = -Z, z = X.
        insv::ImuSample s;
        s.timestamp = kStartUs + uint64_t(std::llround(t * 1e6));
        s.gyro[0] = -w[1], s.gyro[1] = -w[2], s.gyro[2] = w[0];
        s.accel[0] = -up[1], s.accel[1] = -up[2], s.accel[2] = up[0];
        tele.imu.push_back(s);
    }
    return tele;
}
} // namespace lapse

void testTimelapseOrientation()
{
    using namespace lapse;
    const double fps = 30000.0 / 1001.0;
    constexpr int kFrames = 120; // 4 min: frames more than a smoothing window from either end
    const insv::InsvModel &x5 = insv::FindInsvModel("Insta360 X5");
    const insv::ImuTrack track = insv::MakeInsvImuTrack(recorded(kFrames), fps, x5);
    // The stray run is left out; each frame keeps its 11 entries, timed by
    // its centre one.
    CHECK(track.samples.empty() && track.axisMap == x5.timelapseQuatAxes);
    CHECK(track.orientation.size() == size_t(kFrames) * 11);
    CHECK(track.frameTimes.size() == size_t(kFrames));
    CHECK(std::fabs(track.readoutSec - 0.021244) < 1e-12);
    for (int n : {0, 1, 2, 17, kFrames - 1})
        CHECK(std::fabs(track.frameTimes[size_t(n)].videoSec - n / fps) < 1e-12 &&
              std::fabs(track.frameTimes[size_t(n)].trackSec - frameTime(n) - 5 * kStepSec) < 2e-6);

    insv::GyroStabilizer g;
    g.build(track, insv::StabConfig::preset(insv::StabMode::LeanFollow));
    CHECK(g.ready());
    // Each frame's orientation is the camera's then, through the frames' own
    // times: frame 2 sits 4.1 s in, not the 2 x 2.025 s an even spacing gives.
    double worst = 0.0, worstTurn = 0.0;
    for (int n = 0; n < kFrames; ++n) {
        const double t = frameTime(n) - kExposureSec / 2;
        worst = std::max(worst, angle(fromQuat(g.rawOrientation(n / fps)), truth(t)));
        // The turn across the readout, on the camera's clock.
        float r[3];
        g.bodyRotation(n / fps, 0.004, r);
        const M3 d = mul(transposed(truth(t)), truth(t + 0.004));
        const double want[3] = {(d[2][1] - d[1][2]) / 2, (d[0][2] - d[2][0]) / 2, (d[1][0] - d[0][1]) / 2};
        for (int k = 0; k < 3; ++k)
            worstTurn = std::max(worstTurn, std::fabs(r[k] - want[k]));
    }
    CHECK(worst < 1e-6);
    CHECK(worstTurn < 1e-6);
    // The map and its inverse, inside the frames and past either end.
    for (double t : {-1.0, 0.0, 0.05, 0.4, 1.0, 5.0})
        CHECK(std::fabs(g.videoTime(g.trackTime(t)) - t) < 1e-9);
    CHECK(std::fabs(g.trackTime(2 / fps) - g.trackTime(0.0) - 4.1) < 1e-6);
    // A sync fine-tune shifts every frame.
    insv::GyroStabilizer gs;
    gs.build(track, insv::StabConfig::preset(insv::StabMode::LeanFollow), 0.002);
    CHECK(std::fabs(gs.trackTime(3 / fps) - g.trackTime(3 / fps) - 0.002) < 1e-6);

    // Lean follow gives what the gyro's integral of the same motion gives,
    // away from the ends (the window, ~1 min of the camera's clock either
    // side, is cut off there, and the gyro runs on past the frames). What's
    // left, 0.26 deg, is the window read at the frames -- unevenly spaced --
    // against the gyro's even samples.
    const insv::ImuTrack gt = insv::MakeInsvImuTrack(gyro(kFrames), fps, x5);
    CHECK(gt.frameTimes.size() == size_t(kFrames) + 1);
    insv::GyroStabilizer gg;
    gg.build(gt, insv::StabConfig::preset(insv::StabMode::LeanFollow));
    double apart = 0.0;
    for (int n = 35; n < kFrames - 35; ++n) {
        float a[3][3], b[3][3];
        g.correctionMatrix(n / fps, a);
        gg.correctionMatrix(n / fps, b);
        apart = std::max(apart, angle(fromFloat(a), fromFloat(b)));
    }
    std::printf("  timelapse orientation: lean follow %.3f deg from the gyro's\n", apart * 180.0 / 3.141592653589793);
    CHECK(apart < 0.3 * 3.141592653589793 / 180.0);

    // Horizon lock levels the view by the recorded up; Direction lock holds
    // one heading.
    for (insv::StabMode mode : {insv::StabMode::HorizonLock, insv::StabMode::DirectionLock}) {
        insv::GyroStabilizer h;
        h.build(track, insv::StabConfig::preset(mode));
        double tilt = 0.0, minYaw = 1e9, maxYaw = -1e9;
        for (int n = 0; n < kFrames; ++n) {
            float c[3][3];
            h.correctionMatrix(n / fps, c);
            const M3 view = mul(truth(frameTime(n) - kExposureSec / 2), fromFloat(c));
            tilt = std::max(tilt, std::hypot(view[0][1], view[2][1])); // the view's up off +Y
            const double yaw = std::atan2(-view[0][2], -view[2][2]);
            minYaw = std::min(minYaw, yaw);
            maxYaw = std::max(maxYaw, yaw);
        }
        CHECK(tilt < 1e-4);
        if (mode == insv::StabMode::DirectionLock)
            CHECK(maxYaw - minYaw < 1e-4);
        else
            CHECK(maxYaw - minYaw > 1.0); // it follows the 4.9 rad turn
    }

    // No map for the record's axes (a model not measured): no track.
    insv::InsvModel unknown = x5;
    unknown.timelapseQuatAxes = nullptr;
    const insv::ImuTrack none = insv::MakeInsvImuTrack(recorded(kFrames), fps, unknown);
    CHECK(none.orientation.empty() && none.samples.empty());
}

void testPerClipStabilization()
{
    const insv::StabConfig lean = insv::StabConfig::preset(insv::StabMode::LeanFollow);
    const insv::StabConfig level = insv::StabConfig::preset(insv::StabMode::HorizonLock);

    // A new clip takes the setting of the clip before it (after it when it
    // goes first); the first one on an empty timeline starts Off.
    {
        Timeline tl;
        int changed = 0;
        QObject::connect(&tl, &Timeline::clipStabilizationChanged, [&](int, insv::StabConfig) {
            ++changed;
        });
        const int a = tl.appendClip("a.insv", 10000, 30.0);
        CHECK(tl.clipById(a)->stabilization == insv::StabConfig{});
        tl.setClipStabilization(a, lean);
        CHECK(changed == 1);
        tl.setClipStabilization(a, lean); // unchanged: no signal
        CHECK(changed == 1);
        const int b = tl.appendClip("b.insv", 10000, 30.0);
        CHECK(tl.clipById(b)->stabilization == lean);
        tl.setClipStabilization(b, level);
        const int c = tl.insertClip(1, "c.insv", 10000, 30.0); // between a and b
        CHECK(tl.clipById(c)->stabilization == lean);
        const int d = tl.insertClip(0, "d.insv", 10000, 30.0); // first: takes a's
        CHECK(tl.clipById(d)->stabilization == lean);
        CHECK(tl.clipById(b)->stabilization == level); // untouched
        // A split keeps the setting on both halves.
        const int e = tl.splitClip(b, 5000);
        CHECK(e != 0 && tl.clipById(e)->stabilization == level);
    }

    // Undo covers it.
    {
        Project p;
        p.timeline()->appendClip("a.insv", 10000, 30.0);
        p.undo(); // settle
        p.redo();
        const int id = p.timeline()->clipAt(0).id; // undo()/redo() rebuild -> fresh id
        p.timeline()->setClipStabilization(id, level);
        CHECK(p.modified());
        p.undo();
        CHECK(p.timeline()->clipAt(0).stabilization == insv::StabConfig{});
        p.redo();
        CHECK(p.timeline()->clipAt(0).stabilization == level);
    }

    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    // Saved per clip, with no project-wide setting left in the file.
    {
        const QString file = tmp.path() + "/perclip.orp";
        Project p;
        const int a = p.timeline()->appendClip("a.insv", 10000, 30.0);
        p.timeline()->appendClip("b.insv", 10000, 30.0);
        p.timeline()->setClipStabilization(a, level);
        QString err;
        CHECK(p.save(file, &err));
        QFile f(file);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(text.contains("\"version\":10"));
        CHECK(text.count("\"stabilization\"") == 2); // one per clip

        Project q;
        CHECK(q.load(file, &err));
        CHECK(q.timeline()->clipAt(0).stabilization == level);
        CHECK(q.timeline()->clipAt(1).stabilization == insv::StabConfig{});
    }

    // A version 8 (0.1.0) project: its project-wide setting goes onto every
    // clip, and it opens unmodified.
    {
        const QString file = tmp.path() + "/v8.orp";
        writeFile(file, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":8,)"
                            R"("stabilization":{"mode":"horizonLock","horizonLockAmount":0.5},)"
                            R"("timeline":{"clips":[)"
                            R"({"source":"a.insv","sourceDurationMs":1000,"fps":30},)"
                            R"({"source":"b.insv","sourceDurationMs":1000,"fps":30,)"
                            R"("stabilization":{"mode":"leanFollow"}}]}})"));
        Project p;
        QString err;
        CHECK(p.load(file, &err));
        CHECK(err.isEmpty());
        CHECK(!p.modified());
        CHECK(p.timeline()->count() == 2);
        for (int i = 0; i < p.timeline()->count(); ++i) {
            const insv::StabConfig &st = p.timeline()->clipAt(i).stabilization;
            CHECK(st.mode == insv::StabMode::HorizonLock); // a per-clip value is ignored in v8
            CHECK(st.horizonLockAmount == 0.5f);
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); // Project's timers want one (never run here)
    testStabConfig();
    testTimelapseOrientation();
    testPerClipStabilization();
    return modeltest::finish("stabilization_test");
}
