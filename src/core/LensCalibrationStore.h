#pragma once

// The lens calibrations made with Calibrate lenses, one per camera (serial)
// and lens guard, in the app's data folder (<AppData>/calibrations/
// <serial>-<guard>.json, LensCalibrationJson). App-wide, because they belong
// to the camera, not to a project; a clip that uses one keeps its own copy in
// the project (ClipItem::lensCalibration), so this is only where new clips
// take theirs from.

#include <LensCalibration.h>

#include <QString>

#include <optional>
#include <string>

namespace lenscal {

// The folder; created on save.
QString StoreDir();

std::optional<stitch::LensCalibration> Load(const std::string& serial, int lensGuard);
// Replaces the camera's calibration for its guard. False with `error` set if
// it can't be written.
bool Save(const stitch::LensCalibration& c, QString* error = nullptr);
// The calibration a clip can use: its camera's for `lensGuard`, if it was
// measured against the calibration the clip's file carries.
std::optional<stitch::LensCalibration> ForClip(const std::string& serial, int lensGuard,
                                               const insv::LensCalibExt& lens0,
                                               const insv::LensCalibExt& lens1);

} // namespace lenscal
