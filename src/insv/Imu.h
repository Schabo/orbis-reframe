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
// the accel gives it "down".
struct ImuSample {
    // Device clock, microseconds (the X5's raw unit, docs/formats/insta360-x5.md
    // §6.2). Only differences matter: ImuTrack's timing is relative to the
    // earliest sample.
    uint64_t timestamp = 0;
    double accel[3] = {0, 0, 0};  // accelerometer, g   (x, y, z)
    double gyro[3] = {0, 0, 0};   // angular velocity, rad/s (x, y, z)
};

struct ImuTrack {
    std::vector<ImuSample> samples; // sensor axes, any order

    // Sensor axis -> sphere (+X right, +Y up, -Z forward) as a signed
    // permutation (StabConfig::gyroAxes has the syntax). Empty = identity.
    std::string axisMap;

    // Video time t (seconds) is IMU time  t * (1 + clockRate) + offsetSec,
    // seconds after the earliest sample. `hasOffset` false: the file doesn't
    // say, and the stabilizer takes StabConfig::timeOffsetSec as it is.
    bool hasOffset = false;
    double offsetSec = 0.0;
    double clockRate = 0.0; // the camera's frame clock vs nominal video time

    // Sensor row readout time, seconds: row v of a lens was captured
    // (v/height - 0.5) * readoutSec after the time the offset points at. 0 =
    // unknown (no per-row correction).
    double readoutSec = 0.0;
};

} // namespace insv
