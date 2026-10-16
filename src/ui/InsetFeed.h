#pragma once

#include "../core/TrackClip.h" // inset::Look / Frame, InsetKeyTrack
#include "../render/EquirectViewportWindow.h"

#include <QString>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Serves the video tracks' insets to playback: the "second segment"
// next to PlaybackController's main one. One decoder thread keeps, per inset
// clip near the playhead, a few frames decoded ahead
// (exportpipe::FlatVideoReader, RGBA at a capped size); the presenter asks
// for what to draw at its clock time each tick. A playhead jump -- a seek, a
// scrub, or a clip edit that moves the frames -- re-anchors that clip's
// decode at the new time; until the frame for it is ready the previous
// picture stays up.
//
// setClips() may be called from any thread; frameFor() from one presenter
// thread (PlaybackController's).
class InsetFeed {
public:
    // One inset clip, as the model has it. `z`: drawing order (0 = bottom).
    struct Clip {
        int id = 0;
        QString path;
        qint64 startMs = 0;
        qint64 trimInMs = 0;
        qint64 trimOutMs = 0;
        double speed = 1.0;
        double aspect = 16.0 / 9.0;
        qint64 fadeInMs = 0;
        qint64 fadeOutMs = 0;
        inset::Look look;
        inset::Frame frame;
        InsetKeyTrack keys;
        // A flat main-track clip (ClipItem::flat) drawn as a full-frame
        // layer: decoded at a larger size, with its colour grade.
        bool fullFrame = false;
        std::array<float, 4> colorGrade{};

        qint64 spanMs() const { return std::llround((trimOutMs - trimInMs) / speed); }
        qint64 endMs() const { return startMs + spanMs(); }
        qint64 sourceAt(qint64 timelineMs) const
        {
            return trimInMs + std::llround((timelineMs - startMs) * speed);
        }
        qint64 timelineAt(qint64 sourceMs) const
        {
            return startMs + std::llround((sourceMs - trimInMs) / speed);
        }
        // Same decode: the frames it would show are the same.
        bool sameFrames(const Clip &o) const
        {
            return path == o.path && startMs == o.startMs && trimInMs == o.trimInMs &&
                   trimOutMs == o.trimOutMs && speed == o.speed;
        }
    };

    InsetFeed();
    ~InsetFeed();

    // Bottom-most first; at most EquirectViewportWindow::kMaxInsets show at
    // once (the topmost win).
    void setClips(std::vector<Clip> clips);

    // What to draw at timeline time `ms`, and where the decoder should work
    // (ahead of `ms` while `playing`, just at it while paused). Presenter
    // thread.
    std::vector<EquirectViewportWindow::InsetDraw> frameFor(qint64 ms, bool playing);

private:
    struct Frame {
        qint64 timelineMs = 0;
        uint64_t key = 0;
        std::shared_ptr<const std::vector<uint8_t>> rgba;
        int width = 0, height = 0;
    };
    struct Stream;

    void decoderLoop();
    // Decoder thread: one step of work on `s` (a re-anchor, or one frame
    // ahead). False if it had nothing to do.
    bool work(Stream &s, qint64 target, bool playing);
    bool active(const Clip &c, qint64 ms) const;

    std::mutex m_mx;
    std::condition_variable m_cv;
    std::vector<std::shared_ptr<Stream>> m_streams; // guarded by m_mx
    qint64 m_targetMs = 0;
    bool m_playing = false;
    bool m_stop = false;
    uint64_t m_nextKey = 1;
    std::thread m_thread;
};
