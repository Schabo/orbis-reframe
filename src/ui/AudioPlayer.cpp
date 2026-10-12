#include "AudioPlayer.h"

#include <ClipAudioReader.h>

#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
// The X5's audio is fixed-format (48 kHz stereo AAC); InsvAudioDemux
// resamples/remixes to this regardless of what a given clip actually carries,
// so QAudioSink's format never has to change mid-timeline.
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFramesPerMs = kSampleRate / 1000;
// One decoded chunk (~21 ms, an AAC frame's worth); ~0.8 s of lookahead.
constexpr int64_t kChunkFrames = 1024;
constexpr size_t kQueueMaxChunks = 40;
// The sink's own buffer. Everything already in it plays out after a seek (it
// can't be dropped -- see the constructor), so it's kept small; it can be,
// because the sink lives on its own thread and is refilled promptly
// regardless of what the GUI thread is doing. Qt's default is ~250 ms.
constexpr int kSinkBufferMs = 50;
// Drift (video clock vs. what's audible) past this is a jump, not drift --
// e.g. the video clock stepping back after a stall it didn't show as
// "Buffering…": re-seek. The reading itself is steady to ~1 ms.
constexpr qint64 kHardResyncMs = 80;
// Smoothed drift past this (but under kHardResyncMs) is corrected by a splice:
// the read position jumps by the drift -- frames skipped, or silence padded --
// faded out and back in over kFadeFrames so it doesn't click. Below it, drift
// is left alone: well inside lip-sync tolerance, and real clock skew between
// the sound card and the system clock is tens of ppm, so steady playback
// splices rarely if ever. No resampling: a playback-rate nudge (linear
// interpolation at a sweeping sub-sample phase) was tried and audibly warped
// the sound for seconds after a seek.
constexpr double kJumpMs = 10.0;
constexpr int kFadeFrames = 96; // 2 ms
// Smoothing of the per-tick drift reading.
constexpr double kDriftAlpha = 0.2;

} // namespace

// Pull-mode QIODevice: QAudioSink calls readData() whenever its backend wants
// more bytes (on the GUI thread on most Linux backends, but not guaranteed --
// AudioPlayer::pullBytes takes the lock accordingly).
class AudioPlayer::PullDevice : public QIODevice {
public:
    explicit PullDevice(AudioPlayer *owner, QObject *parent = nullptr)
        : QIODevice(parent), m_owner(owner)
    {
    }

protected:
    qint64 readData(char *data, qint64 maxlen) override { return m_owner->pullBytes(data, maxlen); }
    qint64 writeData(const char *, qint64) override { return -1; }
    bool isSequential() const override { return true; }
    // This device always has "data" (real PCM or silence-fill -- see
    // pullBytes) for as long as the sink wants it. Without this override the
    // base QIODevice::bytesAvailable() returns 0, and at least one backend
    // (Qt Multimedia's PipeWire backend) reads that as "nothing to pull" and
    // never calls readData() at all (silent playback).
    qint64 bytesAvailable() const override
    {
        return std::numeric_limits<qint64>::max() - 1024;
    }

private:
    AudioPlayer *m_owner;
};

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent)
{
    QAudioFormat fmt;
    fmt.setSampleRate(kSampleRate);
    fmt.setChannelCount(kChannels);
    fmt.setSampleFormat(QAudioFormat::Float);
    // One sink, started once, for the player's whole life -- only ever
    // suspended/resumed. Never reset()/stop()+start() or recreate it to drop
    // buffered audio: on Qt 6.10's PulseAudio backend, tearing down a stream
    // that hasn't finished connecting (rapid seeks) orphans it on the server
    // until the process exits -- a stream leaked per seek, until
    // pipewire-pulse ran out of file descriptors and refused every client on
    // the desktop. requestSeekLocked() schedules around the buffer instead.
    //
    // It lives on its own thread: in pull mode QAudioSink's backend calls
    // readData() from the thread that owns it, and on the GUI thread any
    // stall longer than the buffer would starve it. There the buffer can be
    // small (kSinkBufferMs), which keeps the unavoidable post-seek tail short.
    m_sinkThread = new QThread(this);
    m_sinkThread->setObjectName(QStringLiteral("AudioSink"));
    m_sinkHost = new QObject;
    m_sinkHost->moveToThread(m_sinkThread);
    m_sinkThread->start();
    QMetaObject::invokeMethod(
        m_sinkHost,
        [this, fmt] {
            m_device = new PullDevice(this, m_sinkHost);
            m_device->open(QIODevice::ReadOnly);
            m_sink = new QAudioSink(fmt, m_sinkHost);
            m_sink->setBufferSize(kSinkBufferMs * kFramesPerMs * kChannels *
                                  static_cast<int>(sizeof(float)));
            m_sink->start(m_device);
            m_sink->suspend(); // nothing playing yet; play() resumes
        },
        Qt::BlockingQueuedConnection);

    startDecoder();
}

AudioPlayer::~AudioPlayer()
{
    // reset(), not stop(): stop() plays out what's buffered first, and Qt
    // 6.8's PulseAudio sink (Debian 13) waits for that forever while the
    // stream is suspended -- quitting while paused hung.
    QMetaObject::invokeMethod(
        m_sinkHost, [this] { m_sink->reset(); }, Qt::BlockingQueuedConnection);
    m_sinkThread->quit();
    m_sinkThread->wait();
    delete m_sinkHost; // and the sink + device, its children
    stopDecoder();
}

void AudioPlayer::startDecoder()
{
    m_decoder = std::thread([this] { decoderLoop(); });
}

void AudioPlayer::stopDecoder()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_decoder.joinable())
        m_decoder.join();
}

// ---------------------------------------------------------------------------
// GUI-thread entry points
// ---------------------------------------------------------------------------

void AudioPlayer::setTimeline(const QList<PlaybackController::Segment> &segments)
{
    QList<Placed> placed = PlaybackController::placeSegments(segments);
    const qint64 totalMs = placed.isEmpty() ? 0 : placed.back().endMs();
    const double played = playedFrames();
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_placed = std::move(placed);
        m_timelineDirty = true;
        // Where the video is now (an edit mid-playback), not the last seek.
        requestSeekLocked(std::clamp<qint64>(m_lastVideoMs, 0, totalMs), played);
    }
    m_cv.notify_all();
}

void AudioPlayer::setMix(std::vector<insv::MixClip> clips)
{
    const double played = playedFrames();
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_mixClips = std::move(clips);
        m_mixDirty = true;
        // Drop the queued audio mixed the old way.
        requestSeekLocked(m_lastVideoMs, played);
    }
    m_cv.notify_all();
}

void AudioPlayer::updateSinkRunning()
{
    bool run;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        run = m_playing; // a hold keeps it running on silence -- see setHold()
    }
    QMetaObject::invokeMethod(m_sinkHost, [this, run] {
        if (run)
            m_sink->resume();
        else
            m_sink->suspend();
    });
}

double AudioPlayer::playedFrames() const
{
    // The sink's playback clock (the sound server's, not bytes handed over --
    // it tracks the video clock to a few ms), in output frames since start().
    // Asked on the sink's thread; the caller mustn't hold m_mx (pullBytes,
    // on that thread, takes it).
    qint64 us = 0;
    QMetaObject::invokeMethod(
        m_sinkHost, [this, &us] { us = m_sink->processedUSecs(); },
        Qt::BlockingQueuedConnection);
    return us * (kSampleRate / 1e6);
}

void AudioPlayer::play()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_playing)
            return;
        m_playing = true;
        m_haveDrift = false;
    }
    m_cv.notify_all();
    updateSinkRunning();
}

void AudioPlayer::pause()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (!m_playing)
            return;
        m_playing = false;
    }
    updateSinkRunning();
}

void AudioPlayer::setHold(bool hold)
{
    // The sink isn't suspended for a hold: pullBytes() feeds it silence
    // instead. That way the few ms of audio already in its buffer finish
    // playing right away (at the seek / stall, where they belong) instead of
    // after the hold, and on release the audio re-seeks to exactly where the
    // video resumes from, behind nothing but silence.
    const double played = playedFrames();
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_held == hold)
            return;
        m_held = hold;
        m_haveDrift = false; // the reading across a stall is meaningless
        if (!hold && m_playing)
            requestSeekLocked(m_lastVideoMs, played);
    }
    m_cv.notify_all();
}

void AudioPlayer::setVolume(float gain)
{
    std::lock_guard<std::mutex> lk(m_mx);
    m_volume = std::clamp(gain, 0.0f, 1.0f);
}

void AudioPlayer::seek(qint64 timelineMs)
{
    const double played = playedFrames();
    {
        std::lock_guard<std::mutex> lk(m_mx);
        requestSeekLocked(timelineMs, played);
    }
    m_cv.notify_all();
}

void AudioPlayer::notePlaybackPosition(qint64 timelineMs)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_lastVideoMs = timelineMs;
    }
    const double played = playedFrames();

    bool resynced = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (!m_playing || m_held || m_seekPending)
            return;
        // A jump isn't audible until the sink plays past where it was made;
        // until then the reading below would count it early.
        if (m_pendingJumpFrames != 0 || played < m_jumpGuardOutFrames)
            return;
        // Pulled but not yet heard, mapped back to timeline frames.
        const double queuedFrames = std::max(0.0, m_outFramesPulled - played);
        const double audibleMs =
            m_seekBaseTimelineMs + (m_srcFramesPlayed - queuedFrames) / kFramesPerMs;
        const double driftMs = timelineMs - audibleMs; // > 0: audio is behind
        if (std::abs(driftMs) > kHardResyncMs) {
            requestSeekLocked(timelineMs, played);
            resynced = true;
        } else {
            m_driftEmaMs = m_haveDrift ? m_driftEmaMs + kDriftAlpha * (driftMs - m_driftEmaMs)
                                       : driftMs;
            m_haveDrift = true;
            if (std::abs(m_driftEmaMs) > kJumpMs) {
                // Splice (pullBytes): fade out, skip (> 0, audio behind) or
                // pad (< 0, ahead), fade in.
                m_pendingJumpFrames = std::llround(m_driftEmaMs * kFramesPerMs);
                m_fadeOutFrames = kFadeFrames;
                m_haveDrift = false;
            }
        }
    }
    if (resynced)
        m_cv.notify_all();
}

void AudioPlayer::requestSeekLocked(qint64 timelineMs, double playedFrames)
{
    // The sink still holds up to a buffer (kSinkBufferMs) of pre-seek audio
    // that can't be dropped (see the constructor) and plays out first, while
    // the video moves on from timelineMs. Start the new audio where the video
    // will be when that runs out, so it lands in step.
    const double bufferedFrames = std::max(0.0, m_outFramesPulled - playedFrames);
    const qint64 targetMs =
        timelineMs + static_cast<qint64>(std::llround(bufferedFrames / kFramesPerMs));
    m_seekPending = true;
    m_seekTargetMs = targetMs;
    m_decoderEof = false;
    ++m_seekGen;
    m_queue.clear();
    m_current = Chunk{};
    m_currentOffset = 0;
    m_haveDrift = false;
    m_pendingJumpFrames = 0;
    m_fadeOutFrames = 0;
    m_fadeInFrames = 0;
    m_srcFramesPlayed = 0.0;
    m_padFrames = 0;
    m_seekBaseTimelineMs = targetMs;
    m_lastVideoMs = timelineMs;
}

// ---------------------------------------------------------------------------
// Decoder thread
// ---------------------------------------------------------------------------

void AudioPlayer::openSegment(int index, int64_t sourceFrame)
{
    const PlaybackController::Segment &seg = m_decPlaced[index].seg;
    if (!m_reader || m_activePath != seg.sourcePath || m_activeTrack != seg.audioTrack) {
        m_reader = std::make_unique<insv::ClipAudioReader>(kSampleRate, kChannels);
        m_reader->open(seg.sourcePath.toStdString(), seg.audioTrack); // none => silence
        m_activePath = seg.sourcePath;
        m_activeTrack = seg.audioTrack;
    }
    m_reader->setGainDb(seg.audioGainDb);
    m_reader->setMuted(seg.audioMuted);
    // ClipItem::speed: stretched to the clip's timeline span, pitch kept.
    m_reader->setTempo(seg.speed);
    m_reader->setNoiseReduction(seg.noise);
    m_reader->seek(sourceFrame);
    m_decSegIndex = index;
}

void AudioPlayer::decoderLoop()
{
    m_mixer = std::make_unique<insv::AudioMixer>(kSampleRate, kChannels);
    for (;;) {
        bool doSeek = false;
        bool tlChanged = false;
        bool mixChanged = false;
        std::vector<insv::MixClip> mix;
        qint64 seekMs = 0;
        uint64_t gen = 0;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this] {
                const bool canDecode = m_playing && !m_decPlaced.isEmpty() && !m_decoderEof &&
                                       m_queue.size() < kQueueMaxChunks;
                return m_stop || m_seekPending || m_timelineDirty || m_mixDirty || canDecode;
            });
            if (m_stop)
                return;
            if (m_timelineDirty) {
                m_decPlaced = m_placed;
                m_timelineDirty = false;
                tlChanged = true;
            }
            if (m_mixDirty) {
                mix = m_mixClips;
                m_mixDirty = false;
                mixChanged = true;
            }
            if (m_seekPending) {
                m_seekPending = false;
                doSeek = true;
                seekMs = m_seekTargetMs;
            }
            gen = m_seekGen;
        }
        if (mixChanged)
            m_mixer->setClips(std::move(mix));

        if (tlChanged && m_decPlaced.isEmpty()) {
            m_reader.reset();
            m_activePath.clear();
            m_decSegIndex = -1;
        }
        if (m_decPlaced.isEmpty())
            continue;

        if (doSeek) {
            const int idx = PlaybackController::segmentAt(m_decPlaced, seekMs);
            const Placed &ps = m_decPlaced[idx];
            const qint64 clamped = std::clamp<qint64>(seekMs, ps.startMs, ps.endMs());
            m_decCursor = clamped * kFramesPerMs;
            openSegment(idx, ps.seg.sourceAtLocalMs(clamped - ps.startMs) * kFramesPerMs);
            std::lock_guard<std::mutex> lk(m_mx);
            m_decoderEof = false;
            continue;
        }

        if (m_decSegIndex < 0 || m_decSegIndex >= m_decPlaced.size())
            continue; // setTimeline() always follows with a seek

        // Up to one chunk, never past this segment's end: the trim-out is cut
        // at the exact frame, and the next segment starts on the next one.
        const Placed &cur = m_decPlaced[m_decSegIndex];
        const int64_t n = std::min<int64_t>(kChunkFrames, cur.endMs() * kFramesPerMs - m_decCursor);
        if (n <= 0) {
            const int next = m_decSegIndex + 1;
            if (next >= m_decPlaced.size()) {
                std::lock_guard<std::mutex> lk(m_mx);
                m_decoderEof = true;
                continue;
            }
            openSegment(next, m_decPlaced[next].seg.inMs * kFramesPerMs);
            continue;
        }

        Chunk chunk;
        chunk.pcm.resize(static_cast<size_t>(n) * kChannels);
        m_reader->read(chunk.pcm.data(), n); // level/mute baked in; silence if no audio
        m_mixer->mixInto(chunk.pcm.data(), m_decCursor, n); // the extra audio tracks
        insv::AudioMixer::limit(chunk.pcm.data(), n * kChannels);
        m_decCursor += n;

        std::lock_guard<std::mutex> lk(m_mx);
        if (m_seekGen != gen)
            continue; // a seek landed while we were decoding -- stale
        m_queue.push_back(std::move(chunk));
    }
}

// ---------------------------------------------------------------------------
// Pull side (QAudioSink's backend, any thread)
// ---------------------------------------------------------------------------

qint64 AudioPlayer::pullBytes(char *data, qint64 maxlen)
{
    const size_t frameBytes = kChannels * sizeof(float);
    auto *out = reinterpret_cast<float *>(data);
    const qint64 framesWanted = maxlen / static_cast<qint64>(frameBytes);

    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto frames = [](const Chunk &c) { return c.pcm.size() / kChannels; };
        if (m_held) { // silence, timeline position untouched -- see setHold()
            std::memset(out, 0, static_cast<size_t>(framesWanted) * frameBytes);
            m_outFramesPulled += static_cast<double>(framesWanted);
            return framesWanted * static_cast<qint64>(frameBytes);
        }
        // A straight copy of the queue -- no resampling. The queue is
        // contiguous on the timeline, so the read position *is* the timeline
        // clock: an underrun plays silence but keeps advancing it, and the
        // late audio is skipped when it arrives -- a short gap, never a
        // lasting lag. Drift corrections (notePlaybackPosition) arrive as a
        // splice, applied here between a fade-out and a fade-in.
        for (qint64 i = 0; i < framesWanted; ++i) {
            if (m_pendingJumpFrames != 0 && m_fadeOutFrames == 0) {
                if (m_pendingJumpFrames > 0) { // behind: skip ahead (rolls across chunks below)
                    m_currentOffset += static_cast<size_t>(m_pendingJumpFrames);
                    m_srcFramesPlayed += static_cast<double>(m_pendingJumpFrames);
                } else { // ahead: hold the read position with silence
                    m_padFrames += -m_pendingJumpFrames;
                }
                m_pendingJumpFrames = 0;
                m_fadeInFrames = kFadeFrames;
                // Readings are stale until the sink has played past the splice.
                m_jumpGuardOutFrames = m_outFramesPulled + static_cast<double>(i + m_padFrames);
            }
            if (m_padFrames > 0) { // a jump back: silence, read position held
                --m_padFrames;
                for (int c = 0; c < kChannels; ++c)
                    out[i * kChannels + c] = 0.0f;
                continue;
            }
            while (m_currentOffset >= frames(m_current) && !m_queue.empty()) {
                m_currentOffset -= frames(m_current);
                m_current = std::move(m_queue.front());
                m_queue.pop_front();
            }
            float gain = m_volume;
            if (m_fadeOutFrames > 0)
                gain *= static_cast<float>(--m_fadeOutFrames) / kFadeFrames;
            else if (m_fadeInFrames > 0)
                gain *= 1.0f - static_cast<float>(--m_fadeInFrames) / kFadeFrames;
            if (m_currentOffset >= frames(m_current)) { // starved
                for (int c = 0; c < kChannels; ++c)
                    out[i * kChannels + c] = 0.0f;
            } else {
                const float *f = m_current.pcm.data() + m_currentOffset * kChannels;
                for (int c = 0; c < kChannels; ++c)
                    out[i * kChannels + c] = f[c] * gain;
            }
            ++m_currentOffset;
            m_srcFramesPlayed += 1.0;
        }
        m_outFramesPulled += static_cast<double>(framesWanted);
    }
    m_cv.notify_all(); // the queue may have room now
    return framesWanted * static_cast<qint64>(frameBytes);
}
