#pragma once

#include <FlatVideoReader.h> // exportpipe::VideoFrame
#include <IStitchSource.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// A crossfade's layer reading playback's own source of its clip, instead of
// opening a second one. The layer's frames lie right next to the ones
// playback plays from the same file, so one source serves both in turn,
// without a seek, and the clip takes the graphics memory once:
// - the first half ("lead-in") blends the next clip in: its frames just
//   before its trim-in, which the source reads before the ones playback
//   plays from the cut. Playback lends it the source sought to the layer's
//   start and takes it back at the cut; the layer's frames not read by
//   then are read first, into a small buffer the layer goes on from.
// - the second half blends the clip before the cut out: its frames just
//   past its trim-out, where playback's source of it stands once it has
//   read the clip's last frame. Playback lends it there and takes it back
//   once the layer is done().
//
// The layer (InsetFeed's thread) asks through the LayerLender; playback's
// decoder answers: lend, decline (paused, no room, not those frames next)
// -- the layer then opens its own -- or later. Every read goes through the
// lease's lock, so the source is only ever read by one thread at a time.
class LayerLease {
public:
    // Renders the next frame of the source as the layer shows it. Called
    // under the lease's lock, from either thread.
    using Render = std::function<bool(stitch::IStitchSource &src, exportpipe::VideoFrame &out)>;

    // Clip `clipId`'s frames from source time `fromMs` up to (not including)
    // `endMs`: up to its trim-in (`leadIn`), else from its trim-out on.
    LayerLease(int clipId, int64_t fromMs, int64_t endMs, bool leadIn, Render render,
               std::function<void()> answered);

    int clipId() const { return m_clipId; }
    int64_t fromMs() const { return m_fromMs; }
    int64_t endMs() const { return m_endMs; }
    bool leadIn() const { return m_leadIn; }

    // --- playback's side ---
    // Lend `src`, sought to fromMs(): its next frame is the layer's first.
    // False when the layer no longer wants it (it left, or it was taken back
    // before the seek was done).
    bool lend(std::shared_ptr<stitch::IStitchSource> src);
    void decline();
    // Still waiting for an answer (the layer hasn't left).
    bool asked() const;
    // Lent, and the layer wants no more from it: it read its last frame, or
    // left.
    bool done() const;
    // The source back (null if it was never lent). With `finish`, the
    // layer's frames still to come are read first, up to kMaxBuffered of
    // them. `atEnd`: the source's next frame is the first at or after
    // endMs() -- playback's, with no seek -- else it needs one.
    std::shared_ptr<stitch::IStitchSource> takeBack(bool finish, bool &atEnd);

    // --- the layer's side ---
    enum class Next {
        Frame,   // `out` is the next frame
        End,     // the layer's frames are all read
        Waiting, // not answered yet: ask again once `answered` is called
        Gone,    // declined, or taken back before the end: the layer reads its own
    };
    Next next(exportpipe::VideoFrame &out);
    // The layer reads no more (it's closing, or reads its own source).
    void leave();

    // The most frames takeBack reads ahead for the layer: a few are left at
    // the cut (the layer reads 4 ahead of the clock, playback 6), more only
    // when the layer stalled -- then it reads its own rather than hold the
    // cut, and RGBA frames at 1080p are 8 MB each.
    static constexpr size_t kMaxBuffered = 15;

private:
    enum class State { Asked, Lent, Declined, Left, Back };
    // Is the source's next frame the layer's (before endMs)? Its time isn't
    // known before it's read: by the frame rate. In doubt (within a ms of
    // the end) not: the layer losing its last frame costs a frame of the
    // blend, playback's first frame read as the layer's costs a seek.
    bool nextIsLayers() const;
    // One frame through m_render; false at the layer's end or on an error.
    bool readOne(exportpipe::VideoFrame &out);

    const int m_clipId;
    const int64_t m_fromMs;
    const int64_t m_endMs;
    const bool m_leadIn;
    std::function<void()> m_answered;

    mutable std::mutex m_mx;
    State m_state = State::Asked;
    Render m_render; // cleared when the layer leaves
    std::shared_ptr<stitch::IStitchSource> m_src;
    double m_frameMs = 1000.0 / 30.0;
    int64_t m_lastMs = -1;    // the last frame read, -1 before the first
    bool m_overran = false;   // a read went past endMs (playback's first frame)
    bool m_failed = false;    // a read failed
    bool m_reachedEnd = false; // taken back with every layer frame read
    std::deque<exportpipe::VideoFrame> m_buffer; // read by takeBack
};

// Where the layers ask: playback's decoder takes the asks and answers each.
// Thread-safe.
class LayerLender {
public:
    // `wakePlayback`: an ask is waiting (playback's decoder may be asleep).
    // `answered`: a lease was lent or declined (the layers re-read).
    LayerLender(std::function<void()> wakePlayback, std::function<void()> answered);

    std::shared_ptr<LayerLease> ask(int clipId, int64_t fromMs, int64_t endMs, bool leadIn,
                                    LayerLease::Render render);
    // The asks not taken yet: the caller lends or declines each.
    std::vector<std::shared_ptr<LayerLease>> takeAsks();
    bool anyAsks() const;

private:
    std::function<void()> m_wake;
    std::function<void()> m_answered;
    mutable std::mutex m_mx;
    std::vector<std::shared_ptr<LayerLease>> m_asks;
};
