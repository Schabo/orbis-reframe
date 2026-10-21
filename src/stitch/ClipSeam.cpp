#include "ClipSeam.h"

#include <Calibration.h>
#include <InsvDemux.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace stitch {

namespace {
constexpr int kSeamFrames = 5;
} // namespace

ClipSeamResult MeasureClipSeam(const std::string& clipPath, int64_t trimInMs, int64_t trimOutMs,
                               PreviewStitchConfig::LensGuard lensGuard,
                               const std::atomic<bool>* cancel)
{
    ClipSeamResult out;
    const auto fail = [&out](MeasureFailure why, std::string message) {
        out.failure = why;
        out.message = std::move(message);
        return out;
    };
    std::string err;
    insv::InsvCalibration calib;
    insv::StitchParams params;
    if (const MeasureFailure why = PrepareClipMeasurement(clipPath, lensGuard, calib, params, err);
        why != MeasureFailure::None)
        return fail(why, err);

    insv::InsvDemux demux;
    if (!demux.open(clipPath, err))
        return fail(MeasureFailure::Unreadable, "demux open failed: " + err);
    if (demux.lensCount() < 2)
        return fail(MeasureFailure::Unsupported, "one lens stream in the file");
    const int w = demux.lens(0).width, h = demux.lens(0).height;
    if (trimOutMs <= trimInMs)
        trimOutMs = demux.lens(0).durationMs;

    // Frames at 10 % .. 90 % of the range: the scene (and so which seam blocks
    // carry texture) changes along a ride.
    std::vector<insv::SeamFrame> frames;
    for (int k = 0; k < kSeamFrames; ++k) {
        if (cancel && cancel->load())
            return fail(MeasureFailure::Cancelled, "cancelled");
        const int64_t t = insv::SnapToKeyframeMs(
            trimInMs + (trimOutMs - trimInMs) * (2 * k + 1) / (2 * kSeamFrames), demux.lens(0).fps);
        insv::Image l0, l1;
        if (!demux.seekTo(t, err) || !demux.readNextPair(l0, l1, nullptr, err))
            continue;
        frames.push_back(insv::MakeSeamFrame(l0, l1));
    }
    if (frames.empty())
        return fail(MeasureFailure::Unreadable, "no frame decoded: " + err);
    static_cast<insv::SeamAlignmentResult&>(out) =
        insv::EstimateSeamAlignment(calib, params, w, h, frames, cancel);
    if (!out.ok)
        out.failure = cancel && cancel->load() ? MeasureFailure::Cancelled
                                               : MeasureFailure::TooLittleDetail;
    return out;
}

} // namespace stitch
