#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <LensColour.h>     // insv::LensColourFit
#include <SeamCorrection.h> // insv::SeamCorrection

#include "ClipMeasure.h"   // stitch::MeasureFailure
#include "IStitchSource.h" // stitch::PreviewStitchConfig::LensGuard

namespace stitch {

struct ClipLensColourResult : insv::LensColourFit {
    MeasureFailure failure = MeasureFailure::None; // why not ok
};

// Measures the clip's lens colour (insv::FitLensColour) for `clipPath` from
// kLensColourFrames lens pairs spread over [trimInMs, trimOutMs], with the
// stitch set up as NativeStitchSource would for `lensGuard` and with the
// clip's seam correction `seam` (the lenses must line up for their colours
// to be compared). CPU decode; several seconds -- run it off the GUI thread.
// `cancel` (optional) is polled between frames.
ClipLensColourResult MeasureClipLensColour(const std::string& clipPath, int64_t trimInMs,
                                           int64_t trimOutMs,
                                           PreviewStitchConfig::LensGuard lensGuard,
                                           const insv::SeamCorrection& seam,
                                           const std::atomic<bool>* cancel = nullptr);

} // namespace stitch
