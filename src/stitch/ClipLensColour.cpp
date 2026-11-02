#include "ClipLensColour.h"

#include <Calibration.h>
#include <FisheyeProjection.h>
#include <InsvDemux.h>
#include <SeamAlignment.h> // insv::SnapToKeyframeMs
#include <SeamCut.h>

#include <memory>
#include <utility>
#include <vector>

namespace stitch {

namespace {
// Flare comes and goes along a ride and the lens difference doesn't follow
// the shutter: enough frames that the ones with flare are a minority to
// leave out, few enough to decode in seconds.
constexpr int kLensColourFrames = 24;
} // namespace

ClipLensColourResult MeasureClipLensColour(const std::string& clipPath, int64_t trimInMs,
                                           int64_t trimOutMs,
                                           PreviewStitchConfig::LensGuard lensGuard,
                                           const LensCalibration* calibration,
                                           const insv::SeamCorrection& seam,
                                           const std::atomic<bool>* cancel)
{
    ClipLensColourResult out;
    const auto fail = [&out](MeasureFailure why, std::string message) {
        out.failure = why;
        out.message = std::move(message);
        return out;
    };
    std::string err;
    insv::InsvCalibration calib;
    insv::StitchParams params;
    std::shared_ptr<const camera::ClipMetadata> clip;
    if (const MeasureFailure why =
            PrepareClipMeasurement(clipPath, lensGuard, calibration, calib, params, err, &clip);
        why != MeasureFailure::None)
        return fail(why, err);
    params.seam = seam;

    insv::InsvDemux demux;
    if (!demux.open(clip->lensSources(), err))
        return fail(MeasureFailure::Unreadable, "demux open failed: " + err);
    if (demux.lensCount() < 2)
        return fail(MeasureFailure::Unsupported, "one lens stream in the file");
    const int w = demux.lens(0).width, h = demux.lens(0).height;
    insv::FisheyeStitcher st;
    if (!st.configure(calib, w, h, params))
        return fail(MeasureFailure::Unsupported, "the stitch can't be set up");
    if (trimOutMs <= trimInMs)
        trimOutMs = demux.lens(0).durationMs;

    // Frames evenly over the range, the lenses sampled raw at the seam (no
    // depth: what doesn't line up at infinity is left out as disagreeing).
    std::vector<std::unique_ptr<insv::SeamCutStats>> stats;
    for (int k = 0; k < kLensColourFrames; ++k) {
        if (cancel && cancel->load())
            return fail(MeasureFailure::Cancelled, "cancelled");
        const int64_t t = insv::SnapToKeyframeMs(
            trimInMs + (trimOutMs - trimInMs) * (2 * k + 1) / (2 * kLensColourFrames),
            demux.lens(0).fps);
        insv::Image l0, l1;
        if (!demux.seekTo(t, err) || !demux.readNextPair(l0, l1, nullptr, err))
            continue;
        auto s = std::make_unique<insv::SeamCutStats>();
        insv::MeasureSeamCutStats(st, l0, l1, nullptr, *s);
        stats.push_back(std::move(s));
    }
    if (stats.empty())
        return fail(MeasureFailure::Unreadable, "no frame decoded: " + err);
    std::vector<const insv::SeamCutStats*> frames;
    for (const auto& s : stats)
        frames.push_back(s.get());
    static_cast<insv::LensColourFit&>(out) = insv::FitLensColour(frames, st.gpuParams().bandRad);
    if (!out.ok)
        out.failure = MeasureFailure::TooLittleDetail;
    return out;
}

} // namespace stitch
