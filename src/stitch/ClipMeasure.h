#pragma once

#include <memory>
#include <string>

#include <ClipMetadata.h> // camera::ClipMetadata

#include "IStitchSource.h" // stitch::PreviewStitchConfig::LensGuard

namespace insv {
struct InsvCalibration;
struct StitchParams;
} // namespace insv

// What the per-clip measurements (MeasureClipSeam, MeasureClipSync) share:
// why one gave no result, and the set-up step both start with.

namespace stitch {

// Why a measurement gave no result, in kinds the app can put in plain words;
// the result's message says it technically, for the log.
enum class MeasureFailure {
    None,            // measured
    Cancelled,
    Unreadable,      // the file, or its frames, can't be read
    NotCamera,       // a video, but not a 360 camera file
    Unsupported,     // a camera file this stitch can't set up (layout, no calibration)
    NoGyro,          // no IMU track, or none the stabilizer can use
    TooLittleDetail, // the picture gave the fit too little to go on
};

// Reads `clipPath`'s metadata and sets its stitch up as NativeStitchSource
// would for `lensGuard`. None on success, with `clip` (optional) set;
// otherwise the kind, with `err`.
MeasureFailure PrepareClipMeasurement(const std::string& clipPath,
                                      PreviewStitchConfig::LensGuard lensGuard,
                                      insv::InsvCalibration& calib, insv::StitchParams& params,
                                      std::string& err,
                                      std::shared_ptr<const camera::ClipMetadata>* clip = nullptr);

} // namespace stitch
