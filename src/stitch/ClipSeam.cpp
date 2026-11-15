#include "ClipSeam.h"

#include <orbis-core/stitch/LensModel.h>
#include <orbis-core/media/FisheyeDemux.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace stitch {

namespace {
constexpr int kSeamFrames = 8;
} // namespace

ClipSeamResult MeasureClipSeam(const std::string& clipPath, int64_t trimInMs, int64_t trimOutMs,
                               PreviewStitchConfig::LensGuard lensGuard,
                               const LensCalibration* calibration,
                               const std::atomic<bool>* cancel)
{
    ClipSeamResult out;
    const auto fail = [&out](MeasureFailure why, std::string message) {
        out.failure = why;
        out.message = std::move(message);
        return out;
    };
    std::string err;
    orbiscore::CameraCalibration calib;
    orbiscore::StitchParams params;
    std::shared_ptr<const camera::ClipMetadata> clip;
    if (const MeasureFailure why =
            PrepareClipMeasurement(clipPath, lensGuard, calibration, calib, params, err, &clip);
        why != MeasureFailure::None)
        return fail(why, err);

    orbiscore::FisheyeDemux demux;
    if (!demux.open(clip->partSources(), err))
        return fail(MeasureFailure::Unreadable, "demux open failed: " + err);
    if (demux.lensCount() < 2)
        return fail(MeasureFailure::Unsupported, "one lens stream in the file");
    const int w = demux.lens(0).width, h = demux.lens(0).height;
    if (trimOutMs <= trimInMs)
        trimOutMs = demux.lens(0).durationMs;

    // Frames at 10 % .. 90 % of the range: the scene (and so which seam blocks
    // carry texture) changes along a ride.
    std::vector<orbiscore::SeamFrame> frames;
    for (int k = 0; k < kSeamFrames; ++k) {
        if (cancel && cancel->load())
            return fail(MeasureFailure::Cancelled, "cancelled");
        const int64_t t = orbiscore::SnapToKeyframeMs(
            trimInMs + (trimOutMs - trimInMs) * (2 * k + 1) / (2 * kSeamFrames), demux.lens(0).fps);
        orbiscore::Image l0, l1;
        if (!demux.seekTo(t, err) || !demux.readNextPair(l0, l1, nullptr, err))
            continue;
        frames.push_back(orbiscore::MakeSeamFrame(l0, l1));
    }
    if (frames.empty())
        return fail(MeasureFailure::Unreadable, "no frame decoded: " + err);
    static_cast<orbiscore::SeamAlignmentResult&>(out) =
        orbiscore::EstimateSeamAlignment(calib, params, w, h, frames, cancel);
    if (!out.ok)
        out.failure = cancel && cancel->load() ? MeasureFailure::Cancelled
                                               : MeasureFailure::TooLittleDetail;
    return out;
}

} // namespace stitch
