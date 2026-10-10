#pragma once

// Subject tracking through the sphere (CLI: tools/track_subject).
//
// The tracker never sees the equirect. Per frame a small rectilinear "virtual
// camera" crop is rendered around the subject through the frame's gyro
// de-rotation -- so a stick swing is gone before the tracker looks, and the
// path comes out in the stabilized frame ReframeTrack keys live in (see
// TrackGeometry.h). A single-object tracker follows the subject in the crop;
// its box centre is turned back into a sphere direction.
//
// Hybrid (the default): OpenCV CSRT tracks -- it held a motorcycle through a
// 20x scale change and a rear -> side -> rear pass where the learned trackers
// lost it -- and VitTrack scores it: the verified crop from a few frames back
// is searched for in a crop centred where CSRT says the subject is now. On
// target that finds it near the centre with a high score; a jump, occlusion or
// drift doesn't, and the frame counts as lost.
//
// The crop holds still (in the stabilized frame) while the box stays near its
// middle, because the trackers keep their box in crop pixels; when the subject
// drifts off-centre or changes size, the crop re-centres on it and the tracker
// is re-inited there on the same frame.
//
// OpenCV (core/imgproc/video/dnn + contrib tracking) is behind a pimpl; this
// header pulls in none of it.

#include "TrackGeometry.h"

#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace track {

enum class TrackerKind { Hybrid, Csrt, Vit, Nano };

struct TrackerParams {
    TrackerKind kind = TrackerKind::Hybrid;
    std::string modelDir;          // empty => defaultModelDir()
    int crop = 512;                // tracking crop, square px
    double fovFactor = 4.0;        // crop FOV = factor * the subject's angular size ...
    double minFovRad = 20.0 * std::numbers::pi / 180.0;  // ... clamped to this
    double maxFovRad = 120.0 * std::numbers::pi / 180.0;
    double recenterFrac = 0.18;    // re-centre when the box centre is this far off (of the crop)
    double lostScore = 0.4;        // score below this = lost
    int verifyCrop = 256;          // hybrid: verifier crop, square px
    double verifyOffFrac = 0.2;    // hybrid: verifier box this far off centre = disagree
    int refLag = 5;                // hybrid: verify against the crop this many good frames back
};

// Where VitTrack / NanoTrack's ONNX files are: $ORBIS_TRACK_MODEL_DIR, else
// the installed (or, in a dev build, the source tree's) models/vittrack --
// core::paths::modelDir.
std::string defaultModelDir();
// The model files the kind needs are there (CSRT needs none).
bool modelsAvailable(const TrackerParams& params);

struct TrackSample {
    int64_t tMs = 0;
    double yaw = 0.0, pitch = 0.0; // stabilized, radians
    double size = 0.0;             // subject angular size (larger side), radians
    double width = 0.0, height = 0.0; // subject angular width / height, radians
    double score = 0.0;            // 0..1 (CSRT alone: 1 while it reports success)
    bool good = false;             // score >= lostScore
    bool reinit = false;           // the crop re-centred on this frame
};

// The sample for the seed frame (what start() was given).
inline TrackSample seedSample(const Seed& seed, int64_t tMs)
{
    TrackSample s;
    s.tMs = tMs;
    s.yaw = seed.yaw;
    s.pitch = seed.pitch;
    s.width = seed.width;
    s.height = seed.height;
    s.size = seed.width > seed.height ? seed.width : seed.height;
    s.score = 1.0;
    s.good = true;
    s.reinit = true;
    return s;
}

class SubjectTracker {
public:
    explicit SubjectTracker(const TrackerParams& params = {});
    ~SubjectTracker();
    SubjectTracker(const SubjectTracker&) = delete;
    SubjectTracker& operator=(const SubjectTracker&) = delete;

    // First frame: `eqRgba` is a 2:1 equirect (w x h RGBA8), `stab` its gyro
    // de-rotation (row-major 3x3; identity when stabilization is off -- it
    // must match what the viewport applies). False (+ err) when a model file
    // is missing or can't be loaded.
    bool start(const uint8_t* eqRgba, int w, int h, const float stab[9], const Seed& seed,
               int64_t tMs, std::string& err);
    // Every later frame, in play order.
    TrackSample step(const uint8_t* eqRgba, int w, int h, const float stab[9], int64_t tMs);

    // The tracking crop of the last start()/step() (BGR, crop x crop) and the
    // tracker's box in it (x, y, w, h), before any re-centre. Only kept when
    // setKeepDebug(true) -- for tools/track_subject.
    void setKeepDebug(bool on);
    const std::vector<uint8_t>& debugCrop() const;
    const int* debugBox() const;
    double debugFovRad() const;

    struct Timing {
        double trackerMs = 0.0, cropMs = 0.0, verifyMs = 0.0; // totals
        int reinits = 0;
    };
    const Timing& timing() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// A tracked path -> sparse reframe keys: lost samples are bridged linearly
// (held at the ends), yaw unwrapped, both axes Gaussian-smoothed over the
// lean-follow window for `smoothStrength` (insv::leanWindowFromStrength), then
// Ramer-Douglas-Peucker keeps the fewest samples whose linear interpolation
// (ReframeTrack::sample's) stays within `tolRad` of the smoothed path.
struct PathKey {
    int64_t tMs = 0;
    double yaw = 0.0, pitch = 0.0;
};
struct ReduceParams {
    float smoothStrength = 0.2f;
    double tolRad = 0.5 * std::numbers::pi / 180.0;
};
std::vector<PathKey> reduceToKeys(const std::vector<TrackSample>& samples, double fps,
                                  const ReduceParams& params = {},
                                  std::vector<PathKey>* smoothed = nullptr);

} // namespace track
