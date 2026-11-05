// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A camera's gyro track as the stabilizer takes it, whichever camera recorded
// it: the raw samples, how the sensor's axes turn into the stitched sphere's,
// and how video time lines up with the samples' clock. Each camera's reader
// fills one (for Insta360 files: MakeInsvImuTrack, InsvImu.h); the stabilizer
// knows nothing about the file it came from.

namespace insv {

// One inertial-measurement sample: the camera's accelerometer + gyroscope at a
// point in time. GyroStabilizer integrates these into a per-frame orientation;
// the accel gives it "down" (unless the track has ImuTrack::orientation).
struct ImuSample {
    // Device clock, microseconds (the X5's raw unit, docs/formats/insta360-x5.md
    // §6.2). Only differences matter: ImuTrack's timing is relative to the
    // earliest sample.
    uint64_t timestamp = 0;
    double accel[3] = {0, 0, 0};  // accelerometer, g   (x, y, z)
    double gyro[3] = {0, 0, 0};   // angular velocity, rad/s (x, y, z)
};

// How the camera sat in the world at a point in time, from a source that
// knows it better than the IMU alone -- a drone's flight controller, whose
// accelerometer reads the thrust along its tilted body in sustained flight
// and whose heading doesn't drift as the gyro's does -- or from the camera's
// own fusion where it stores that instead of the gyro (the X5's timelapse).
// Two world directions in the track's sensor axes (as ImuSample::accel; the
// axes ImuTrack::axisMap maps), unit vectors.
struct OrientationSample {
    uint64_t timestamp = 0; // the ImuSample clock, microseconds
    double up[3] = {0, 0, 0};
    // A horizontal direction fixed in the world (the flight controller's
    // north, which needn't be true north; the camera's fusion's, which turns
    // as its heading drifts); all zero where the source knows only up.
    double north[3] = {0, 0, 0};
};

// Where a video frame sits on the track's clock (ImuTrack::frameTimes).
struct FrameTime {
    double videoSec = 0.0; // the frame's video time
    double trackSec = 0.0; // the time the correction is computed for, as offsetSec
};

struct ImuTrack {
    std::vector<ImuSample> samples; // sensor axes, any order

    // The camera's orientation over the clip, in time order; empty when the
    // camera doesn't record it. Where present the stabilizer levels against
    // its up instead of the accelerometer, and holds its heading to its
    // north instead of the gyro alone. A track with an orientation and no
    // samples is stabilized from the orientation alone: a timelapse whose
    // camera stores its orientation across each frame's readout instead of
    // the gyro (the X5's), where there is nothing between the frames to
    // integrate. Its times are then relative to its earliest sample, as the
    // samples' would be.
    std::vector<OrientationSample> orientation;

    // Sensor axis -> sphere (+X right, +Y up, -Z forward) as a signed
    // permutation (StabConfig::gyroAxes has the syntax). Empty = identity.
    std::string axisMap;

    // Video time t (seconds) is IMU time
    //   t * captureScale * (1 + clockRate) + offsetSec,
    // seconds after the earliest sample. `hasOffset` false: the file doesn't
    // say, and the stabilizer takes StabConfig::timeOffsetSec as it is.
    bool hasOffset = false;
    double offsetSec = 0.0;
    // Seconds of the camera's clock per second of video, by what the mode
    // says: 1, but 60 in a timelapse of one frame every 2 s at 29.97 and
    // 0.2997 in 100 fps slow motion stored at 29.97 (insv::CaptureScale).
    double captureScale = 1.0;
    double clockRate = 0.0; // the camera's frame clock vs that, a few ppm

    // Each frame's own time, where the file records it (a timelapse's), in
    // video order: video time maps to track time by straight lines between
    // these instead of the formula above, and by the formula, shifted onto
    // them, beyond their ends. A timelapse's frames needn't be evenly spaced
    // (the X5 skips a 100 ms tick now and then), and the formula's straight
    // line can then be most of an interval off. offsetSec is the track time
    // of video 0 by these. Empty: the formula alone.
    std::vector<FrameTime> frameTimes;

    // Sensor row readout time, seconds: row v of a lens was captured
    // (v/height - 0.5) * readoutSec after the time the offset points at. 0 =
    // unknown (no per-row correction).
    double readoutSec = 0.0;
};

} // namespace insv
