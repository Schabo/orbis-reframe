// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "Imu.h"
#include "InsvModels.h"
#include "InsvTrailer.h"

#include <string>

namespace insv {

// Lines an Insta360 trailer's IMU up with the video (docs/formats/insta360-x5.md
// §7.1) and hands it over as the stabilizer's ImuTrack, taking the samples out
// of `tele`.
//
// The IMU starts before recording (sensor pre-roll, 0.16-0.78 s on the
// samples); metadata field 24 is the device timestamp of video frame 0, so the
// offset is (field24 - imu[0].timestamp)/1e6 + field28/1e3, plus the TimeMap
// trim offset on files trimmed by Insta360 software. Verified against image
// motion on two clips. With field 24 it also fits the camera frame clock's
// rate against the nominal `fps` from the exposure record (the MP4 PTS are
// nominal; the device clock runs ~+40 / -15 ppm off, ~20 ms by 8 min). Both
// sensors are read out top to bottom over the readout time (field 25), in
// step (a per-cell survey on four clip segments); the offset targets the
// sensor's centre row, and readoutSec brings every row to that instant
// (stitch::CaptureTiming). Without field 24 it falls back to
// median(exposure[n] - imu[0] - n/fps), which assumes exposure[0] is frame 0
// and is off by the pre-roll.
//
// The axis map is the model's (InsvModel::imuAxes). A drone's attitude
// record (InsvTelemetry::attitude) becomes ImuTrack::orientation through field 69 and
// the model's tilt (InsvModel::imuMount, attitudeTiltDeg); field 69 missing,
// the model's value stands in and that is logged.
ImuTrack MakeInsvImuTrack(InsvTelemetry&& tele, double fps, const InsvModel& model);

} // namespace insv
