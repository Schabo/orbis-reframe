#pragma once

// PlaybackController's tuning, shared by its two source files: the GUI /
// present side (PlaybackController.cpp) and the decoder thread
// (PlaybackControllerDecoder.cpp). Private: only those include it.

#include <QtGlobal>

#include <algorithm>
#include <cstddef>

namespace playbacktuning {

// How many decoded frames to buffer ahead during playback: up to kQueueMax
// (~0.4 s at 30 fps, enough to ride out a decode hiccup), but no more than
// kQueueBudgetBytes of RGBA -- a 5760x2880 Auto preview frame is 66 MB, so
// that's 7 frames, and 4 at 7680. See queueCapFor().
constexpr size_t kQueueMax = 12;
constexpr size_t kQueueMin = 4;
constexpr size_t kQueueBudgetBytes = size_t(512) << 20;
// While paused/scrubbing there's no clock to run ahead of, so only decode a
// frame or two past the one being shown.
constexpr size_t kScrubLookahead = 2;
// Treat "within one frame of the end" as the end.
constexpr qint64 kEndEpsilonMs = 20;
// A seek target this close to a queued frame's time is that frame: the two
// round a frame's start differently (Timeline::stepFrames rounds up, the
// decoder's pts to nearest).
constexpr qint64 kSameFrameMs = 2;
// A scrub this little ahead of the decoder decodes forward to the exact frame
// (~8 ms a frame at 8K) instead of landing on a keyframe.
constexpr qint64 kScrubDecodeAheadMs = 300;
// A scrub's pointer resting this long gets the exact frame (settle()).
constexpr int kScrubSettleMs = 150;
// A scrub going back stops on a keyframe only if it's this far behind where
// the decoder is (which runs a frame or two past the frame on screen).
constexpr qint64 kScrubHoldMs = 100;
// Present cadence. Decoupled from decode / clip frame rate -- it just governs
// how often the wall clock is checked and the shown frame swapped.
constexpr int kPresentIntervalMs = 16;
// After a seek, play, or stall the clock stays frozen until this many frames
// are queued (~0.27 s at 30 fps), not just the first one: a stitch that has
// just warmed up runs barely ahead of real time, and resuming at once made it
// run dry again tick after tick -- a stuttering picture, and choppy audio --
// for about a second after every seek.
constexpr size_t kResumeFrames = 8;

// Lens frames are ring slots on the GPU, not RAM: queue at most this many,
// which with what the viewport holds stays inside the source's ring
// (VulkanFisheyeStitcher, 10 slots before it waits).
constexpr size_t kLensQueueMax = 6;

// Playback queue length for frames of `frameBytes` (0 = none decoded yet).
inline size_t queueCapFor(size_t frameBytes)
{
    if (frameBytes == 0)
        return kQueueMax;
    return std::clamp(kQueueBudgetBytes / frameBytes, kQueueMin, kQueueMax);
}

// Seek the next clip's source this long (timeline) before its boundary, on a
// worker: a seek decodes from the keyframe up to the trim-in, ~0.3 s for an
// 8K clip, which the decoder can't do in time while the queue holds ~0.2 s.
constexpr qint64 kBoundaryPrefetchMs = 1500;

} // namespace playbacktuning
