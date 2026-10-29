// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Imu.h" // insv::ImuSample, insv::ImuTrack

// GyroStabilizer integrates the camera's own IMU track into a smoothed
// orientation and produces a per-frame de-rotation to apply at reprojection
// time. The correction lives in the camera-body (= stitched sphere) frame, so
// it composes AFTER the reframe pose: d_sphere = C * P * d_view (applying it to
// the view ray before P turns its axes with the view -- wrong for any reframed
// look direction; measured 2026-09-25). No Qt, no Vulkan, and nothing about
// any camera's file: its input is an insv::ImuTrack (Imu.h) that the
// camera's reader lined up with the video.
//
// Modes over one mechanism:
//   - LeanFollow    : smooth the fast rotational shake, keep the slow
//                     intentional motion -- crucially a motorcycle's cornering
//                     lean.
//   - HorizonLock   : additionally drive roll toward level.
//   - DirectionLock : Horizon lock whose heading doesn't follow the camera's
//                     turns but holds one direction in the world.

namespace insv {

enum class StabMode {
    Off,
    LeanFollow,
    HorizonLock,
    Custom, // caller sets the windows / lock amount directly
    DirectionLock,
};

struct StabConfig {
    StabMode mode = StabMode::Off;

    // Per-axis smoothing half-window, seconds. Larger = steadier, less follow.
    // preset() fills these; Custom lets the UI drive them.
    float windowYawSec = 0.4f;
    float windowPitchSec = 0.4f;
    float windowRollSec = 0.4f;

    // 0 = keep the followed (leaned) orientation; 1 = a level virtual camera
    // (zero pitch/roll against gravity, heading smoothed over windowYawSec).
    // Blends in between. > 0 also turns on build()'s weak, gravity-seeded
    // accel anchor that keeps the track's world +Y on "up" (2026-09-25
    // redesign); a track with ImuTrack::orientation anchors on its up
    // instead, more firmly, and holds its heading to its north. Used by the HorizonLock preset and available in Custom.
    float horizonLockAmount = 0.0f;

    // Scales the accel-anchor gain in build() (1 = ~20 s time constant). Only
    // used when horizonLockAmount > 0; 0 = pure gyro (the horizon then drifts
    // with gyro bias). Only matters when the IMU track carries accel.
    float horizonGain = 1.0f;

    // Low-pass cutoff (Hz) applied to the accelerometer before it feeds the
    // Mahony horizon-fusion term in build(). Gravity is DC, but the stick mount
    // puts ~0.4-0.6 g of ~11 Hz resonance on the accel; without this that junk
    // leaks straight into the "leveled" orientation and HorizonLock injects
    // 4-14 Hz jitter on every axis. A one-pole IIR at this cutoff keeps the
    // slow leveling and rejects the shake band. 0 = off (raw accel). Only
    // matters with an accel track and horizonGain > 0.
    float accelLowPassHz = 0.0f;

    // Level the view against the mount's mean gravity direction (the clip's
    // mean accelerometer vector): a tilted mount then reads level, and only
    // departures from its average posture -- a cornering lean -- tilt the
    // view. Without it Lean follow's "up" is the camera body's, so a tilted
    // stick tilts the horizon. Needs an accel track; a track with
    // ImuTrack::orientation takes the mean of its up instead (a drone's mean posture
    // against true gravity, not against its thrust). No effect at
    // horizonLockAmount 1 (already level).
    bool autoLevel = true;

    // Hold one heading in the world instead of following the camera's turns:
    // the heading the camera faced (smoothed over windowYawSec) at video
    // source time directionSec. Turns the level (horizonLockAmount) or
    // followed orientation about world up, so pitch and roll are as without
    // it. The gyro alone gives the heading, so it drifts with the gyro's
    // residual bias over long clips. Used by the DirectionLock preset.
    bool lockDirection = false;
    float directionSec = 0.0f;

    // Constant offset added to the virtual-camera orientation, for a tilted
    // camera mount (radians).
    float pitchOffsetRad = 0.0f;
    float rollOffsetRad = 0.0f;

    // The axis map, scale and time offset are developer knobs
    // (tools/residual_stab); the .orp doesn't store them.

    // gyro body axis -> world (+X right, +Y up, -Z forward) as a signed
    // permutation, e.g. "y,z,x" or "-y,x,-z". Token k (k=0,1,2 => world X,Y,Z)
    // names the source gyro component feeding world axis k, optionally negated.
    // Empty => the track's own (ImuTrack::axisMap) in build(ImuTrack, ...),
    // "x,y,z" in build(samples, ...). Set only to try another map.
    std::string gyroAxes;
    float gyroScale = 1.0f;

    // Seconds to add to a video source-time to get the matching gyro time,
    // measured from the earliest IMU sample. 0 = the track's own
    // (build(ImuTrack, ...)); ~+0.16..+0.78 s on the X5 samples.
    float timeOffsetSec = 0.0f;

    // Fills windows / lock amount for a named mode; leaves axis map / offsets.
    static StabConfig preset(StabMode m);

    bool operator==(const StabConfig& o) const
    {
        return mode == o.mode && windowYawSec == o.windowYawSec &&
               windowPitchSec == o.windowPitchSec && windowRollSec == o.windowRollSec &&
               horizonLockAmount == o.horizonLockAmount && horizonGain == o.horizonGain &&
               accelLowPassHz == o.accelLowPassHz && autoLevel == o.autoLevel &&
               lockDirection == o.lockDirection && directionSec == o.directionSec &&
               pitchOffsetRad == o.pitchOffsetRad && rollOffsetRad == o.rollOffsetRad &&
               gyroAxes == o.gyroAxes && gyroScale == o.gyroScale &&
               timeOffsetSec == o.timeOffsetSec;
    }
    bool operator!=(const StabConfig& o) const { return !(*this == o); }
};

// StabMode <-> stable string tokens, for JSON / config.
inline const char* stabModeToString(StabMode m)
{
    switch (m) {
    case StabMode::LeanFollow: return "leanFollow";
    case StabMode::HorizonLock: return "horizonLock";
    case StabMode::Custom: return "custom";
    case StabMode::DirectionLock: return "directionLock";
    case StabMode::Off: break;
    }
    return "off";
}
inline StabMode stabModeFromString(const std::string& s)
{
    if (s == "leanFollow")
        return StabMode::LeanFollow;
    if (s == "horizonLock")
        return StabMode::HorizonLock;
    if (s == "custom")
        return StabMode::Custom;
    if (s == "directionLock")
        return StabMode::DirectionLock;
    return StabMode::Off;
}

// A sensor vector in the sphere's axes: a signed permutation, written as
// StabConfig::gyroAxes is ("-y,x,-z"). Token k names the sensor component
// that feeds sphere axis k.
struct AxisMap {
    int idx[3] = {0, 1, 2};
    double sign[3] = {1, 1, 1};

    void apply(const double in[3], double out[3]) const
    {
        for (int k = 0; k < 3; ++k)
            out[k] = sign[k] * in[idx[k]];
    }
};
// Empty or malformed: identity.
AxisMap ParseAxisMap(const std::string& s);

// Lean-follow "Strength" (0..1) <-> the shared per-axis smoothing window, seconds.
// The UI exposes one Strength slider instead of three raw windows; this is the
// single place that mapping lives. Monotonic and mutually inverse: 0 => a short
// window (0.2 s, maximum follow, barely stabilised), 1 => a long window (5 s,
// heavy smoothing for very shaky footage -- it starts to flatten a slow
// cornering lean too). 0.5 is the LeanFollow default (0.9 s); a power curve
// through that point gives finer control at the low end. Clamp-safe on both
// directions.
float leanWindowFromStrength(float strength01);
float leanStrengthFromWindow(float windowSec);

// Horizon-lock "Smoothing" (0..1) <-> the heading window (windowYawSec),
// seconds: how long the level camera holds its heading before following a
// turn. 0 => 0.3 s (tracks the road closely), 1 => 10 s (a very calm view that
// lags in corners); 0.5 is the HorizonLock default (1.5 s). Same curve shape
// and clamping as the Lean-follow mapping.
float horizonWindowFromStrength(float strength01);
float horizonStrengthFromWindow(float windowSec);

class GyroStabilizer {
public:
    // Integrate raw IMU samples into an orientation track under `cfg`, with
    // cfg.gyroAxes as the full axis map and cfg.timeOffsetSec as the
    // video->IMU offset (nominal video time, no readout). Cheap (~1 kHz, a few
    // hundred k samples); safe to call again to rebuild on a settings change.
    // A track with < 2 samples, or mode == Off, leaves the stabilizer
    // not-ready (identity).
    void build(const std::vector<insv::ImuSample>& imu, const StabConfig& cfg);

    // As above, on a camera's track lined up with the video by its reader:
    // its axis map, video->IMU offset, frame-clock rate and readout time, and
    // its orientation (ImuTrack::orientation) where it has one.
    // Video time t maps to gyro time  t * (1 + clockRate) + timeOffsetSec.
    // If cfg.timeOffsetSec is non-zero it is an explicit override and used
    // as-is (the clock rate still applies); a non-empty cfg.gyroAxes replaces
    // the track's map. `syncAdjustSec` is added to the track's offset (not to
    // an override): the clip's measured fine-tune (insv::SyncEstimator /
    // stitch::MeasureClipSync), since the file only pins the sync to a few ms.
    void build(const ImuTrack& track, const StabConfig& cfg, double syncAdjustSec = 0.0);

    // How far the camera turned between video times tSec and tSec + dtSec, as
    // a rotation vector (axis * angle, rad) in the body (= stitched sphere)
    // frame at tSec: R(t)^T R(t + dt) = exp(r). From the built orientation
    // track, so it uses the same sync and axis map as the correction. Feeds
    // the stitch's per-sensor-row rolling-shutter correction, which needs the
    // real rotation across the ~21 ms readout -- the stick's ~11 Hz swing
    // turns a quarter cycle in that time, so a single rate doesn't fit it.
    // Zero when !ready().
    void bodyRotation(double tSec, double dtSec, float r[3]) const;

    // The video->IMU offset (seconds) build() actually used -- the track's,
    // or cfg.timeOffsetSec when that was non-zero / the track had none. For
    // diagnostics.
    float effectiveTimeOffsetSec() const { return m_cfg.timeOffsetSec; }
    // The track's frame-clock rate (build(ImuTrack, ...) only; 0 otherwise),
    // parts per million. For diagnostics.
    double clockRatePpm() const { return m_clockRate * 1e6; }

    // Sensor row readout time, seconds: row v of either lens was captured
    // (v/height - 0.5) * readoutSec() after the time the correction is
    // computed for. The track's (build(ImuTrack, ...)); 0 otherwise (no
    // per-row correction).
    double readoutSec() const { return m_readoutSec; }

    // Same smoothing + correction pipeline as build(), but the orientation
    // track is supplied directly: body->world unit quaternions {w,x,y,z} at
    // times `tSec` (seconds, monotonic, video-source-relative). For image-based
    // stabilization and IMU/optical fusion, which produce the
    // track by other means. `tSec` and `qBodyToWorld` must be the same length
    // (>= 2); mode == Off or a length mismatch leaves it not-ready (identity).
    void buildFromOrientation(const std::vector<double>& tSec,
                              const std::vector<std::array<double, 4>>& qBodyToWorld,
                              const StabConfig& cfg);

    bool ready() const { return m_ready; }
    StabMode mode() const { return m_cfg.mode; }

    // Row-major 3x3 de-rotation for a frame at video source-time `tSec`, in the
    // camera-body (sphere) frame. Identity when !ready(). It applies to the
    // world ray AFTER the reframe pose:  M_final = correctionMatrix * M_reframe.
    void correctionMatrix(double tSec, float m[3][3]) const;

    // The integrated (unsmoothed) body->world orientation {w,x,y,z} at video
    // source-time `tSec`, through the same sync as correctionMatrix(). Identity
    // when !ready(). For diagnostics / offline smoothing experiments.
    std::array<double, 4> rawOrientation(double tSec) const;

private:
    // Both build()s: `ref` is the track's ImuTrack::orientation (empty:
    // level against the accelerometer, the heading the gyro's alone).
    void buildTrack(const std::vector<insv::ImuSample>& imu,
                    const std::vector<OrientationSample>& ref, const StabConfig& cfg);

    bool m_ready = false;
    StabConfig m_cfg;
    std::vector<double> m_t;                  // sample time, seconds, gyro-relative
    std::vector<std::array<double, 4>> m_qRaw; // integrated body->world orientation {w,x,y,z}
    // Body direction whose horizontal heading the horizon-lock camera follows:
    // body forward with the mount's mean "up" removed (set by build()).
    std::array<double, 3> m_levelFwd{0.0, 0.0, -1.0};
    // Body-frame rotation taking the view frame onto (m_levelFwd, mean up):
    // the auto-level offset (StabConfig::autoLevel). {w,x,y,z}.
    std::array<double, 4> m_mountLevel{1.0, 0.0, 0.0, 0.0};
    // Device frame clock vs nominal video time: 0 = nominal.
    double m_clockRate = 0.0;
    double m_readoutSec = 0.0; // see readoutSec()
    // build()'s integrated, levelled track, kept across rebuilds: it depends
    // on the IMU track and its up reference, the axis map / scale / accel
    // low-pass / gain and whether leveling is on -- not on the windows or lock amount the
    // Stabilization panel's sliders change (the 10 min ride: ~400 ms to build).
    struct TrackCache {
        bool valid = false, fuse = false;
        size_t n = 0;
        uint64_t tFirst = 0, tLast = 0;
        std::string axes;
        float scale = 0.0f, lowPassHz = 0.0f, gain = 0.0f;
        size_t upN = 0;
        uint64_t upFirst = 0, upLast = 0;
        std::vector<double> t;
        std::vector<std::array<double, 4>> q;
        std::array<double, 3> levelFwd{0.0, 0.0, -1.0};
        std::array<double, 4> mountLevel{1.0, 0.0, 0.0, 0.0};
    } m_cache;

    // The camera's heading about world up (radians) at gyro time `t`: m_levelFwd
    // carried through the track and projected onto the horizontal plane,
    // Gaussian-smoothed over windowYawSec.
    double smoothedHeading(double t) const;
    // Sets m_lockedHeading from directionSec once the track, m_levelFwd and
    // the clock rate are final (the end of every build).
    void lockHeading();
    double m_lockedHeading = 0.0; // StabConfig::lockDirection's heading, radians

    // Video source time -> the orientation track's (gyro-relative) time.
    double gyroTime(double tSec) const { return tSec * (1.0 + m_clockRate) + m_cfg.timeOffsetSec; }
};

// --- generic rotation helpers (float; shared with the reframe compose) -------
// Convention matches src/export/ReframeRenderer.cpp:  world = Ry(yaw) Rx(pitch) Rz(roll) v .

void YawPitchRollToMatrix(float yaw, float pitch, float roll, float m[3][3]);
void MatrixToYawPitchRoll(const float m[3][3], float& yaw, float& pitch, float& roll);
void MatrixMul(const float a[3][3], const float b[3][3], float out[3][3]); // out = a * b

} // namespace insv
