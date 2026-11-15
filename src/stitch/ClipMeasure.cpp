#include "ClipMeasure.h"

#include "NativeStitchSource.h" // stitch::DualFisheyeStitchSetup

#include <orbis-core/insta360/Calibration.h>

namespace stitch {

MeasureFailure PrepareClipMeasurement(const std::string& clipPath,
                                      PreviewStitchConfig::LensGuard lensGuard,
                                      const LensCalibration* calibration,
                                      orbiscore::InsvCalibration& calib,
                                      orbiscore::StitchParams& params, std::string& err,
                                      std::shared_ptr<const camera::ClipMetadata>* clip)
{
    camera::ReadFailure why = camera::ReadFailure::Unreadable;
    const auto meta = camera::ReadClipMetadata(clipPath, &err, &why);
    if (!meta)
        return why == camera::ReadFailure::NotCamera ? MeasureFailure::NotCamera
                                                     : MeasureFailure::Unreadable;
    if (!DualFisheyeStitchSetup(*meta, lensGuard, calibration, false, calib, params, err))
        return MeasureFailure::Unsupported;
    if (clip)
        *clip = meta;
    return MeasureFailure::None;
}

} // namespace stitch
