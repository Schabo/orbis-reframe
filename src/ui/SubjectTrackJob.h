#pragma once

// "Track subject" worker (Reframe panel): runs track::SubjectTracker over one
// clip from the playhead, off the GUI thread, and hands every tracked frame
// out as it goes so MainWindow can play it back in the viewport (the camera
// following the subject, the box drawn over it) while the user watches and
// decides when to stop. MainWindow draws the seed box and writes the reduced
// path into the clip's ReframeTrack. Only built with subject tracking
// (ORBIS_REFRAME_HAVE_TRACKING).

#include <SubjectTracker.h> // track::Seed, TrackSample

#include <NativeStitchSource.h> // stitch::PreviewStitchConfig, stitch::Frame

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace insv {
class GyroStabilizer;
}

namespace subjecttrack {

struct Request {
    std::string clipPath;
    int64_t fromMs = 0; // source time: the playhead
    int64_t toMs = 0;   // source time: the clip's trim-out
    double fps = 30.0;
    track::Seed seed;
    // The clip's gyro de-rotation, as the viewport applies it; null => off.
    std::shared_ptr<const insv::GyroStabilizer> stab;
    stitch::PreviewStitchConfig::LensGuard lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
    std::array<double, 3> seamAdjustDeg{0.0, 0.0, 0.0};
    int workWidth = 3840;     // analysis equirect width (2:1)
    bool realTime = true;     // no faster than the footage plays, so it can be watched
    double maxLostSec = 0.0;  // stop after this long without a verified frame; 0 = never
    // Privacy blur, so the frames it plays back in the viewport are blurred
    // like playback's (IStitchSource::SetBlurQuads); empty => none.
    stitch::BlurQuadsFn blur;
};

struct Result {
    std::vector<track::TrackSample> samples; // up to and including the last good frame
    bool endedLost = false;                  // it was lost when it ended (samples cut there)
    bool stopped = false;                    // the user stopped it
    std::string error;
    double fpsAchieved = 0.0; // tracking capacity (pacing sleeps left out)
};

// One tracked frame: the stitched equirect (moved out -- the callback owns
// it), its gyro de-rotation, and where the subject is.
using FrameFn = std::function<void(stitch::Frame&& frame, const float stab[9],
                                   const track::TrackSample& sample)>;

// Blocking. `onFrame` is called for every frame, from the calling thread;
// `stop` is polled every frame.
Result run(const Request& req, const std::atomic<bool>& stop, const FrameFn& onFrame);

} // namespace subjecttrack
