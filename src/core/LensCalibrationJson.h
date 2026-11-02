#pragma once

// A camera's own lens calibration (stitch::LensCalibration) as JSON: the
// form the app keeps them in (one file per camera and lens guard) and the
// copy a project keeps for each clip that uses one. Numbers are written
// with full precision, so a calibration read back stitches identically.

#include <LensCalibration.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace lenscal {

nlohmann::json ToJson(const stitch::LensCalibration& c);
// Nothing for anything that isn't a whole calibration of this version.
std::optional<stitch::LensCalibration> FromJson(const nlohmann::json& j);

// "none" | "standard" | "premium" (stitch::PreviewStitchConfig::LensGuard).
const char* GuardName(int lensGuard);
int GuardFromName(const std::string& name); // -1 if unknown

} // namespace lenscal
