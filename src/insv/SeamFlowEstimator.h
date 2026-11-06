#pragma once

// The seam flow's estimate (SeamFlow.h): OpenCV's DIS optical flow between
// the two lenses' strips, both ways. Part of insv_optical, so the stitch
// core stays OpenCV-free; without OpenCV there is no seam flow.

#include "SeamFlow.h"

#include <memory>

namespace insv {

class SeamFlowEstimator {
public:
    SeamFlowEstimator();
    ~SeamFlowEstimator();
    SeamFlowEstimator(const SeamFlowEstimator&) = delete;
    SeamFlowEstimator& operator=(const SeamFlowEstimator&) = delete;

    // The flow both ways between `s`'s strips into `out` (on s.grid): F10
    // and F01 in radians of (phi, e), on the strips anchored
    // (AnchorSeamFlowStrips). Each frame on its own, no warm start.
    // False (out empty) for invalid strips.
    bool estimate(const SeamFlowStrips& s, SeamFlowField& out);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace insv
