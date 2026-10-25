#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include "PlaybackController.h" // reuses PlaybackController::Segment / Placed

#include <AudioMixer.h> // insv::MixClip

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AudioOutput;

// Plays a timeline's audio track in sync with PlaybackController's video clock.
// A *slave*, not a second master clock: MainWindow forwards it the same
// setTimeline/play/pause calls it sends the controller, seeks it on
// PlaybackController::seeked (in order with the ticks), holds it (on
// silence) while the video clock is frozen (setHold, driven by
// PlaybackController::clockHeldChanged), and every
// PlaybackController::positionChanged tick calls notePlaybackPosition(). That
// compares the video clock with what's audible (AudioOutput::playedFrames(),
// the sound server's playback clock) and corrects in two tiers, never by
// resampling: a faded splice of the read position (skip or pad) once the
// smoothed drift passes ~10 ms, and a re-seek for a real jump. The output is
// never flushed: a seek lets its ~30 ms buffer play out and starts the new
// audio where the video will be by then.
//
// The decoder emits a sample-exact timeline: the main track's clips are
// silence of their exact length (their sound is on the audio tracks), so the
// PCM stream is contiguous and lines up with the timeline by construction.
//
// Threads: the public API is GUI-thread. A background std::thread decodes
// into a bounded queue of PCM chunks; the AudioOutput's own thread drains it
// (pullFrames) whenever the sound server wants more, so GUI stalls can't
// starve its small buffer. A mutex guards everything shared.
//
// Without a sound output (no sound server) the player stays silent and
// idle -- no decoding -- and the video plays on its own clock.
//
// Everything heard is on the audio tracks (setMix) -- the video clips' own
// streams too, as linked clips: summed by an insv::AudioMixer on the decoder
// thread, then the whole mix goes through AudioMixer::limit -- the same code
// the export runs.
class AudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    // Same segment shape as PlaybackController::setTimeline (see there) --
    // MainWindow calls both from pushTimelineToController(). Safe to call
    // repeatedly (a trim edit, a project (re)load); keeps roughly the same
    // playback position, clamped to the new total.
    void setTimeline(const QList<PlaybackController::Segment> &segments);
    // The extra audio tracks' clips, in timeline frames at 48 kHz.
    // Replaces the previous set; re-seeks so the change is heard at once.
    void setMix(std::vector<insv::MixClip> clips);

    void play();
    void pause();
    void seek(qint64 timelineMs);
    // Call on every PlaybackController::positionChanged. Compares the
    // controller's timeline position against what's audible now and splices
    // the read position toward it past kJumpMs; re-seeks past kHardResyncMs.
    // No-op while paused, held, or mid-seek.
    void notePlaybackPosition(qint64 timelineMs);
    // Freeze output while the video clock is frozen (a seek / stall) so the
    // two resume together instead of drifting apart.
    void setHold(bool hold);
    // The preview's listening volume, a linear gain (0 = silent, 1 = as
    // mixed): the player bar's volume and mute. Applied at the output only --
    // never the export or the clips' levels.
    void setVolume(float gain);

    // Whether there's a sound output to play to (AudioOutput::ok()), and one
    // line describing it, or why there's none.
    bool hasOutput() const;
    std::string outputDescription() const;

private:
    using Placed = PlaybackController::Placed;
    // One decoded chunk of interleaved float32 PCM (kChannels-wide). Chunks
    // are contiguous on the timeline from the last seek point.
    struct Chunk {
        std::vector<float> pcm;
    };

    void startDecoder();
    void stopDecoder();
    void decoderLoop();
    // Caller holds m_mx; playedFrames from playedFrames(), taken before locking.
    void requestSeekLocked(qint64 timelineMs, double playedFrames);
    // GUI thread. The output runs iff playing (a hold keeps it running on
    // silence -- see setHold()).
    void updateOutputRunning();
    // The output's playback clock in output frames since it opened. Caller
    // must not hold m_mx.
    double playedFrames() const;
    void pullFrames(float *out, uint32_t frames); // AudioOutput's thread

    std::unique_ptr<AudioOutput> m_out;

    // --- shared, guarded by m_mx ---
    std::mutex m_mx;
    std::condition_variable m_cv;
    QList<Placed> m_placed;
    bool m_timelineDirty = false;
    std::vector<insv::MixClip> m_mixClips;
    bool m_mixDirty = false;
    bool m_playing = false;
    bool m_held = false;
    float m_volume = 1.0f; // setVolume()
    bool m_stop = false;
    bool m_decoderEof = false;
    bool m_seekPending = false;
    qint64 m_seekTargetMs = 0;
    uint64_t m_seekGen = 0;
    std::deque<Chunk> m_queue;
    Chunk m_current;             // the chunk pullFrames() is part-way through
    size_t m_currentOffset = 0;  // frames (not floats) already consumed from m_current
    // A pending drift splice (notePlaybackPosition -> pullFrames): frames to
    // skip (> 0) or pad with silence (< 0), applied once m_fadeOutFrames has
    // run out, followed by m_fadeInFrames of fade-in.
    int64_t m_pendingJumpFrames = 0;
    int m_fadeOutFrames = 0;
    int m_fadeInFrames = 0;
    double m_driftEmaMs = 0.0;
    bool m_haveDrift = false;
    // This player's own playback clock: exact at every seek/resync
    // (m_seekBaseTimelineMs), advanced by the timeline frames handed to the
    // output since then (m_srcFramesPlayed) -- see pullFrames().
    // Advances through an underrun (silence) too, so it stays a true clock.
    qint64 m_seekBaseTimelineMs = 0;
    double m_srcFramesPlayed = 0.0;
    double m_outFramesPulled = 0.0; // output frames pulled since the output opened (cumulative)
    qint64 m_lastVideoMs = 0;       // last notePlaybackPosition() -- where a hold parks
    int64_t m_padFrames = 0;        // silence to output before reading on (a jump back)
    double m_jumpGuardOutFrames = 0.0; // ignore drift until the output has played this far
    std::thread m_decoder;

    // --- decoder thread only ---
    QList<Placed> m_decPlaced;
    int m_decSegIndex = -1;
    int64_t m_decCursor = 0; // next timeline frame the decoder emits
    std::unique_ptr<insv::AudioMixer> m_mixer;
};
