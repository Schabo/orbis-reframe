// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "Imu.h" // insv::ImuTrack

#include <cstddef>
#include <vector>

// How the vehicle the camera rides on moves, from the camera's IMU: its lean
// and the acceleration it feels, along and across the way it travels. For
// the overlays (a lean gauge, a G circle) and for lining the GPS up with the
// video (AlignGpsByAcceleration, Gps.h). No Qt, nothing about any camera's
// file: the input is the track its reader lined up with the video.
//
// The camera needn't face the way it travels -- on the X5 samples it sits
// 20-70 degrees off it, and tilted. The tilt is the camera's mean posture
// over the clip; the direction of travel is the level axis the camera tilts
// about as the vehicle turns (a vehicle leans into a turn: the tilt across
// the travel follows the turn rate), fitted once per clip. Where the clip
// hardly turns (or the fit finds no such axis) the camera's own forward is
// taken.
namespace insv {

struct MotionTrack {
    // One value per step of video time, the first at 0 s; NaN where the IMU
    // has no samples.
    double stepSec = 0.01;
    // Roll about the direction of travel, degrees, positive leaning right,
    // 0 at the camera's mean posture. Smoothed over ~0.25 s: what's left of a
    // stick mount's swing is well under a degree.
    std::vector<float> leanDeg;
    // Acceleration in the level plane, in g (9.80665 m/s^2): across the
    // travel, positive toward the right (turning right), and along it,
    // positive speeding up. Smoothed over ~0.5 s: a stick mount shakes the
    // accelerometer by ~0.35 g RMS above 2 Hz.
    std::vector<float> lateralG;
    std::vector<float> forwardG;
    // How well the turn rate follows the tilt about the fitted travel axis
    // (a correlation, 0..1); 0 when the camera's forward was taken.
    double travelFit = 0.0;

    bool empty() const { return leanDeg.empty(); }
    size_t size() const { return leanDeg.size(); }
};

// The track's motion over its video, empty with fewer than two samples or
// none with an accelerometer. Levels the camera as Horizon lock does
// (GyroStabilizer, its default settings -- not the clip's own, so the values
// don't change with the Stabilization panel). ~0.5 s for a 4 min X5 clip.
MotionTrack BuildMotion(const ImuTrack& track);

} // namespace insv
