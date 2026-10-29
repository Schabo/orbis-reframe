// Stabilization: insv::StabConfig and the gyro stabilizer on synthetic IMU
// data (lean follow, horizon lock, direction lock, auto-level, readout), and
// the per-clip setting on the Timeline and in the project file.
// Standalone (no event loop: same-thread signals fire inside emit); the
// exit code is the failure count.

#include "support/ModelTest.h"

#include <InsvImu.h>    // insv::MakeInsvImuTrack
#include <InsvModels.h> // insv::FindInsvModel

#include <QCoreApplication>

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
    testPerClipStabilization();
    return modeltest::finish("stabilization_test");
}
