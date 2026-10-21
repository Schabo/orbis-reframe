#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <SeamAlignment.h> // insv::SeamAlignmentResult

#include "ClipMeasure.h"   // stitch::MeasureFailure
#include "IStitchSource.h" // stitch::PreviewStitchConfig::LensGuard

namespace stitch {

struct ClipSeamResult : insv::SeamAlignmentResult {
    MeasureFailure failure = MeasureFailure::None; // why not ok
};

// Measures the seam alignment fine-tune (insv::EstimateSeamAlignment) for
// `clipPath` from kSeamFrames lens pairs spread over [trimInMs, trimOutMs],
// with the stitch set up as NativeStitchSource would for `lensGuard`. CPU
// decode; a few seconds -- run it off the GUI thread. `cancel` (optional) is
// polled between frames and fit steps.
ClipSeamResult MeasureClipSeam(const std::string& clipPath, int64_t trimInMs, int64_t trimOutMs,
                               PreviewStitchConfig::LensGuard lensGuard,
                               const std::atomic<bool>* cancel = nullptr);

} // namespace stitch
