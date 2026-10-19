#include "ClipSeam.h"

#include "NativeStitchSource.h" // stitch::DualFisheyeStitchSetup

#include <Calibration.h>
#include <ClipMetadata.h>
#include <InsvDemux.h>

#include <algorithm>
#include <vector>

namespace stitch {

namespace {
constexpr int kSeamFrames = 5;
} // namespace

insv::SeamAlignmentResult MeasureClipSeam(const std::string& clipPath, int64_t trimInMs,
                                          int64_t trimOutMs,
                                          PreviewStitchConfig::LensGuard lensGuard,
                                          const std::atomic<bool>* cancel)
{
    insv::SeamAlignmentResult out;
    std::string err;
    const auto clip = camera::ReadClipMetadata(clipPath, &err);
    if (!clip) {
        out.message = err;
        return out;
    }
    insv::InsvCalibration calib;
    insv::StitchParams params;
    if (!DualFisheyeStitchSetup(*clip, lensGuard, calib, params, err)) {
        out.message = err;
        return out;
    }

    insv::InsvDemux demux;
    if (!demux.open(clipPath, err) || demux.lensCount() < 2) {
        out.message = "demux open failed: " + err;
        return out;
    }
    const int w = demux.lens(0).width, h = demux.lens(0).height;
    if (trimOutMs <= trimInMs)
        trimOutMs = demux.lens(0).durationMs;

    // Frames at 10 % .. 90 % of the range: the scene (and so which seam blocks
    // carry texture) changes along a ride.
    std::vector<insv::SeamFrame> frames;
    for (int k = 0; k < kSeamFrames; ++k) {
        if (cancel && cancel->load()) {
            out.message = "cancelled";
            return out;
        }
        const int64_t t = insv::SnapToKeyframeMs(
            trimInMs + (trimOutMs - trimInMs) * (2 * k + 1) / (2 * kSeamFrames), demux.lens(0).fps);
        insv::Image l0, l1;
        if (!demux.seekTo(t, err) || !demux.readNextPair(l0, l1, nullptr, err))
            continue;
        frames.push_back(insv::MakeSeamFrame(l0, l1));
    }
    return insv::EstimateSeamAlignment(calib, params, w, h, frames, cancel);
}

} // namespace stitch
