#include "ClipSeam.h"

#include "NativeStitchSource.h" // stitch::LensGuardProfileName

#include <Calibration.h>
#include <InsvDemux.h>
#include <InsvTrailer.h>

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
    const auto notes = insv::ReadInsvMakerNotes(clipPath);
    if (!notes) {
        out.message = "no readable trailer";
        return out;
    }
    insv::InsvCalibration calib;
    const auto ext = insv::ReadInsvExtendedCalibrationString(clipPath);
    if (!ext || !insv::ParseExtendedCalibrationString(*ext, calib)) {
        out.message = "no extended (MEI) calibration";
        return out;
    }
    insv::StitchParams params;
    insv::ApplyLensProfile(params, *notes, LensGuardProfileName(lensGuard));

    insv::InsvDemux demux;
    std::string err;
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
