#include "ClipMeasure.h"

#include "NativeStitchSource.h" // stitch::DualFisheyeStitchSetup

#include <orbis-core/stitch/LensModel.h>
#include <orbis-core/stitch/SeamAlignment.h> // orbiscore::SnapToKeyframeMs

namespace stitch {

MeasureFailure PrepareClipMeasurement(const std::string& clipPath,
                                      PreviewStitchConfig::LensGuard lensGuard,
                                      const LensCalibration* calibration,
                                      orbiscore::CameraCalibration& calib,
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

int64_t MeasureFrameMs(const camera::ClipMetadata& clip, int64_t ms, double fps)
{
    return clip.photoCount > 0 ? ms : orbiscore::SnapToKeyframeMs(ms, fps);
}

} // namespace stitch
