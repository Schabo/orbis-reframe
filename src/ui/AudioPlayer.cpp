#include "AudioPlayer.h"

#include "AudioOutput.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
// The mixer resamples / remixes every clip to the output's format, so it
// never has to change mid-timeline.
constexpr int kSampleRate = AudioOutput::kSampleRate;
constexpr int kChannels = AudioOutput::kChannels;
constexpr int kFramesPerMs = kSampleRate / 1000;
// One decoded chunk (~21 ms, an AAC frame's worth); ~0.8 s of lookahead.
constexpr int64_t kChunkFrames = 1024;
constexpr size_t kQueueMaxChunks = 40;
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

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent)
{
    // One stream for the player's whole life, only ever started and
    // stopped; requestSeekLocked() schedules around what's buffered in it.
    // Never a new stream to drop buffered audio: every stream costs the
    // sound server and its watchers (pavucontrol opens a level meter per
    // stream), and rapid churn has exhausted pipewire-pulse's file
    // descriptors, which takes sound away from the whole desktop.
    m_out = std::make_unique<AudioOutput>([this](float *out, uint32_t frames) { pullFrames(out, frames); });
    if (m_out->ok())
        startDecoder();
}

AudioPlayer::~AudioPlayer()
{
    m_out.reset(); // stops the callback before the queue it reads goes
    stopDecoder();
}

bool AudioPlayer::hasOutput() const
{
    return m_out->ok();
}

std::string AudioPlayer::outputDescription() const
{
    return m_out->description();
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

void AudioPlayer::updateOutputRunning()
{
    bool run;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        run = m_playing; // a hold keeps it running on silence -- see setHold()
    }
    // Not under m_mx: stopping waits for the callback, which takes it.
    if (run)
        m_out->start();
    else
        m_out->stop();
}

double AudioPlayer::playedFrames() const
{
    // The sound server's clock, not frames handed over (it tracks the video
    // clock to a few ms). Takes the output's own lock, never m_mx.
    return m_out->playedFrames();
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
    updateOutputRunning();
}

void AudioPlayer::pause()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (!m_playing)
            return;
        m_playing = false;
    }
    updateOutputRunning();
}

void AudioPlayer::setHold(bool hold)
{
    // The output isn't stopped for a hold: pullFrames() feeds it silence
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
        // A jump isn't audible until the output plays past where it was made;
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
                // Splice (pullFrames): fade out, skip (> 0, audio behind) or
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
    // The output still holds up to a buffer (~30 ms) of pre-seek audio that
    // isn't dropped (miniaudio has no flush) and plays out first, while
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

        if (tlChanged && m_decPlaced.isEmpty())
            m_decSegIndex = -1;
        if (m_decPlaced.isEmpty())
            continue;

        if (doSeek) {
            const int idx = PlaybackController::segmentAt(m_decPlaced, seekMs);
            const Placed &ps = m_decPlaced[idx];
            const qint64 clamped = std::clamp<qint64>(seekMs, ps.startMs, ps.endMs());
            m_decCursor = clamped * kFramesPerMs;
            m_decSegIndex = idx;
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
            m_decSegIndex = next;
            continue;
        }

        Chunk chunk;
        chunk.pcm.assign(static_cast<size_t>(n) * kChannels, 0.0f);
        m_mixer->mixInto(chunk.pcm.data(), m_decCursor, n); // the audio tracks
        insv::AudioMixer::limit(chunk.pcm.data(), n * kChannels);
        m_decCursor += n;

        std::lock_guard<std::mutex> lk(m_mx);
        if (m_seekGen != gen)
            continue; // a seek landed while we were decoding -- stale
        m_queue.push_back(std::move(chunk));
    }
}

// ---------------------------------------------------------------------------
// Pull side (AudioOutput's thread)
// ---------------------------------------------------------------------------

void AudioPlayer::pullFrames(float *out, uint32_t frames)
{
    const size_t frameBytes = kChannels * sizeof(float);
    const qint64 framesWanted = frames;

    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto frames = [](const Chunk &c) { return c.pcm.size() / kChannels; };
        if (m_held) { // silence, timeline position untouched -- see setHold()
            std::memset(out, 0, static_cast<size_t>(framesWanted) * frameBytes);
            m_outFramesPulled += static_cast<double>(framesWanted);
            return;
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
                // Readings are stale until the output has played past the splice.
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
}
