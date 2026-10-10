#pragma once

// Number-plate scan for the privacy blur.
//
// Frame by frame, in play order, from a FrameSource that renders views of
// the frame on demand (StitchFrameSource: straight from the fisheyes on the
// GPU), levelled with the gyro (Horizon lock: detectors are trained on
// upright photos):
//   1. a ring of rectilinear tiles around the levelled horizon (12 x 40 deg
//      at pitch -10 by default, 608 px each) is rendered into one mosaic;
//   2. the plate detector (YOLOv9 plate model, open-image-models, MIT; ncnn
//      on Vulkan) runs on the whole mosaic at once -- every frame: detecting
//      every Nth frame loses plates even with tracking;
//   3. each box goes back onto the sphere; duplicates from overlapping tiles
//      merge;
//   4. detections are linked into tracks frame to frame (constant angular
//      velocity gate);
//   5. a quad tracker follows each plate past its detections (LK features
//      in a zoomed levelled view around it -> homography -> the four
//      corners), verified face-on against the plate at its last detection.
//      It carries the blur through a close side-on pass where the detector
//      is blind.
// finish() then drops tracks with too few detections, fills gaps up to
// maxGap frames (where no quad covers them), holds every track a few frames
// before / after, and maps every region back into the stitch's sphere frame
// (PlateScanData.h).
//
// Pipelined so the detector never waits: while it works on frame f, the
// tracker's quads take their step on f (provisionally -- a detection on f
// replaces it) and frame f+1's tiles are rendered; once f is settled, f+1's
// quad views are rendered and its detection starts. The quad views are
// chosen before the frame's detection is known: each live quad's, plus one
// around every plate detected on the frame before -- so a new track gets its
// quad from its second detection, and a detection re-anchors its quad in the
// view already rendered. A view moved between frames is an exact homography
// (same centre of projection), so no frame is ever rendered twice.
//
// Unlike the spike, a new track on top of a live quad doesn't end it -- two
// overlapping blurs beat a dropped one -- and the frame a quad is lost on is
// covered by its hold.
//
// OpenCV and ncnn are behind a pimpl; this header pulls in neither.

#include "PlateScanData.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace privacy {

struct ScanParams {
    // ncnn .param (its .bin beside it); empty => defaultModelParam(). The
    // model is cut before its box decoding (any input size; three heads,
    // out0..out2 = strides 8 / 16 / 32, 64 DFL + 1 class channels).
    std::string modelParam;
    // Tiles.
    int tiles = 12;
    double tileFovDeg = 40.0;
    double tilePitchDeg = -10.0;
    int tilePx = 608; // the size the model was trained at (yolo-v9-s-608)
    // Detection.
    float conf = 0.25f;
    double maxBoxDeg = 15.0; // wider boxes are dropped (a whole car rear, never a plate)
    bool gpu = true;         // ncnn on Vulkan; false = its CPU path
    // Tracks / fill.
    int maxGap = 15; // interpolate across misses up to this many frames
    int lead = 3, tail = 8;
    int minHits = 3; // tracks with fewer detections are dropped
    double dilateW = 1.3, dilateH = 1.5;
    // Quad tracker.
    int quadMax = 90;      // frames a quad may run past its last detection
    double quadNcc = 0.5;  // verification threshold
    int quadMaxUnverified = 6, quadHold = 5;
};

// "Blur this plate" (manual add, PlateScanner::follow): one plate the user
// boxed, followed from that frame on. Each frame the detector runs on one
// zoomed tile around where the plate is predicted (the plate ~platePx
// across; a lower threshold is fine there, nothing else is looked at) and
// re-anchors the quad tracker; between detections the quad carries it.
struct FollowParams {
    float conf = 0.1f;
    int tilePx = 640; // a multiple of 32
    // ~25 px: zoomed further the plate gets soft and the model misses it
    // (cars clip, 4 plates: 75-117 frames detected at 25 px, 14-68 at 60,
    // 1-50 at 100).
    double platePx = 25.0;
    double minTileFovDeg = 6.0, maxTileFovDeg = 70.0;
    int maxWithoutDetection = 300; // quad-tracked frames in a row (vs the scan's 90)
    int hold = 10; // frames held where it was lost, still looking for it there
};

struct FollowResult {
    // From the first frame on, one region per frame (track 0), sphere frame;
    // ends with the hold frames when the plate was lost.
    std::vector<ScanFrame> frames;
    bool lost = false; // ended because the plate was lost (else the range ended or it was stopped)
    int detections = 0;
};

// $ORBIS_PLATE_MODEL, else <model>.ncnn.param in the installed (or, in a dev
// build, the source tree's) models/plates -- core::paths::modelDir.
std::string defaultModelParam();
// The .param and its .bin are there.
bool modelAvailable(const std::string& param);

// A view of the frame, in the levelled frame: centred at lon / lat (deg; lon
// 0 = forward, + right; lat + up), hfovDeg wide, no roll, rendered w x h at
// (x, y) of the canvas.
struct ScanView {
    double lonDeg = 0, latDeg = 0, hfovDeg = 40;
    int x = 0, y = 0, w = 0, h = 0;
};

// Where the scanner's pictures come from.
class FrameSource {
public:
    virtual ~FrameSource() = default;
    // Move on to the next frame: its source time and levelling matrix
    // (row-major 3x3, levelled direction -> the stitch's sphere direction:
    // insv::GyroStabilizer::correctionMatrix in Horizon lock). False at the
    // end (err empty) or on error.
    virtual bool next(int64_t& tMs, float level[9], std::string& err) = 0;
    // `views` of the current frame into a canvasW x canvasH RGBA8 canvas
    // (the rest grey, 114). As often as wanted until the next next() /
    // release().
    virtual bool render(int canvasW, int canvasH, const std::vector<ScanView>& views,
                        std::vector<uint8_t>& rgba, std::string& err) = 0;
    // Done with the current frame (it may be let go).
    virtual void release() {}
};

class PlateScanner {
public:
    PlateScanner();
    ~PlateScanner();
    PlateScanner(const PlateScanner&) = delete;
    PlateScanner& operator=(const PlateScanner&) = delete;

    // Loads the model. False (+ err) when it can't.
    bool init(const ScanParams& params, std::string& err);
    // "Vulkan: <device>" or "CPU", after init.
    std::string device() const;

    // Every frame `src` gives, until it ends or `stop` is set. `progress`
    // (optional) gets each settled frame's time. False (+ err) on a source
    // error; what was scanned is kept either way.
    bool run(FrameSource& src, const std::atomic<bool>* stop,
             const std::function<void(int64_t tMs)>& progress, std::string& err);

    // Tracks -> regions per frame, sphere frame. `fps` is the clip's.
    PlateScan finish(double fps);

    // "Blur this plate": follow the plate inside `seed` (a quad in the sphere
    // frame of `src`'s first frame, roughly around the plate) frame by frame
    // until it is lost, `src` ends or `stop` is set. `progress` (optional)
    // gets each frame's time and whether the detector found the plate on it.
    // False (+ err) on a source error before the first frame. Independent of
    // run() / finish().
    bool follow(FrameSource& src, const insv::BlurQuad& seed, const FollowParams& fp,
                const std::atomic<bool>* stop, const std::function<void(int64_t tMs, bool detected)>& progress,
                FollowResult& out, std::string& err);

    struct Stats {
        int frames = 0;
        int detections = 0;  // raw, after merging tiles
        int tracks = 0;      // linked (finish: before dropping short ones)
        double renderMs = 0; // totals: tiles + quad views on the source
        double detectMs = 0; //   the detector (its own thread)
        double waitMs = 0;   //   waiting for the detector
        double quadMs = 0;   //   quad steps + anchors
        double wallMs = 0;
    };
    const Stats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace privacy
