#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// The sound card, through miniaudio (the only file that includes it): one
// playback stream on the system's default output, 48 kHz stereo float,
// opened once and kept for the player's life. miniaudio loads the sound
// server's library (libpulse, ALSA, JACK) at run time, so nothing is linked
// and a system without sound still runs -- ok() is then false.
//
// The stream pulls: the callback runs on miniaudio's own thread whenever the
// server wants more, and must fill every frame it's asked for. On the
// PulseAudio backend (PipeWire's pipewire-pulse too) the stream follows the
// default output and can be moved between outputs while it plays.
class AudioOutput {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;

    // Fill `frames` interleaved frames at `out`. miniaudio's thread.
    using Pull = std::function<void(float *out, uint32_t frames)>;
    // System: the sound server (PulseAudio, then ALSA, then JACK).
    // Null: miniaudio's null device, which pulls in real time and plays
    // nothing -- for tests.
    enum class Backend { System, Null };

    explicit AudioOutput(Pull pull, Backend backend = Backend::System);
    ~AudioOutput();
    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;

    // Whether a stream is open. Without one start() / stop() do nothing and
    // playedFrames() stays 0.
    bool ok() const;
    // One line for the log and Help > About: the backend and device, or why
    // there's no sound.
    const std::string &description() const;

    // Start / stop pulling; blocking, GUI thread, never from the callback.
    // On PulseAudio stop() corks the stream: what's buffered stays and plays
    // first on the next start(). ALSA drops it. Either way the clock below
    // stays right, and there's no flush: the buffer is ~30 ms.
    void start();
    void stop();

    // Frames heard since the stream opened -- the sound server's playback
    // clock, which counts the output device's latency (5 ms on a USB headset,
    // 100-200 ms over Bluetooth), not frames handed over. Comparable with the
    // count of frames pulled: pulled minus played is what's on its way.
    // Any thread; never blocks on the sound server.
    double playedFrames() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};
