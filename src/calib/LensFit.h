#pragma once

// A camera's lens calibration from a clip of the chart (ChartDetect.h).
//
// The model is the stitch's (insv::LensProjection, MEI with radial,
// tangential and thin-prism terms), started from the file's own calibration.
// First each lens alone (the board's pose per frame and the lens's
// parameters), then both together with the rotation from lens 0 to lens 1
// and one board pose per frame (a bundle adjustment, Levenberg-Marquardt
// with the poses taken out by the Schur complement, Huber weights, outliers
// dropped). Fixed at the file's: xi, and the lenses' spacing -- the board is
// always too near (0.5-1 m) to tell the spacing from the lenses' scale
// towards the rim. Only frames where the board holds still are used, so
// the rolling shutter doesn't matter. Whatever was over the lenses when the
// chart was filmed (a lens guard) ends up in the fitted parameters.

#include "ChartDetect.h"

#include <LensCalibration.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace camera {
struct ClipMetadata;
}

namespace calib {

struct LensFitOptions {
    double squareMm = 0;          // the chart's square as it was shown
    double stillPxPerFrame = 1.5; // the most the board may move, px per frame
    int minCorners = 12;          // per lens and frame
    // What was over the lenses (stitch::PreviewStitchConfig::LensGuard as
    // int): the file's model is compared with the fit as the app would
    // stitch the clip without the calibration, the guard's profile on top.
    int lensGuard = 0;
};

struct LensFitResult {
    bool ok = false;
    std::string message; // why not, when !ok
    // lenses, fileLenses, squareMm, rmsPx, transferDeg, coverageDeg; the
    // caller sets serial, lensGuard and created.
    stitch::LensCalibration calibration;
    int framesUsed[2] = {0, 0};
    int framesBoth = 0;
    // The file's own model (with the guard's profile, as the app stitches
    // without the calibration) on the same corners, only the poses fitted.
    double fileRmsPx[2] = {0, 0};
    double fileTransferDeg = 0;
    // How far the new calibration moves the picture against the file's
    // (with the guard's profile): per lens, content where the lenses meet
    // (90 deg off its axis), median and most, degrees; and the lens 0 ->
    // lens 1 rotation's change.
    double shiftDeg[2] = {0, 0};
    double shiftMaxDeg[2] = {0, 0};
    double rotationChangeDeg = 0;
};

// `clip`: the chart clip's metadata (its file calibration is the start and
// the reference); `obs`: its chart corners. `progress` 0..1.
LensFitResult FitLensCalibration(const camera::ClipMetadata& clip, const ChartObservations& obs,
                                 const LensFitOptions& opt,
                                 const std::function<void(double)>& progress = {},
                                 const std::atomic<bool>* cancel = nullptr);

// What a calibration must reach to be kept: corners out to the rim on both
// lenses, a close fit, and the two lenses agreeing on where the board is.
constexpr double kMinCoverageDeg = 85;
constexpr double kMaxRmsPx = 0.35;
constexpr double kMaxTransferDeg = 0.05;
constexpr int kMinFrames = 150; // per lens

enum class LensFitProblem {
    TooFewFrames,  // a lens saw the still board in fewer than kMinFrames frames
    CoverageLens0, // the board never reached far enough out in lens 0
    CoverageLens1,
    Rms,           // the model doesn't fit the corners closely enough
    Transfer,      // the lenses disagree about where the board is
};
std::vector<LensFitProblem> CheckLensFit(const LensFitResult& r);

} // namespace calib
