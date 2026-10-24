// src/ui/AudioOutput on miniaudio's null device (real-time pulls, no sound
// server): pulls arrive only while started, the clock advances only while
// started and never ahead of what was pulled, and rapid start / stop and
// open / close cycles neither hang nor stop the pulls.

#include "AudioOutput.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct Counter {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> calls{0};
    AudioOutput::Pull pull()
    {
        return [this](float *out, uint32_t n) {
            for (uint32_t i = 0; i < n * AudioOutput::kChannels; ++i)
                out[i] = 0.0f;
            frames += n;
            ++calls;
        };
    }
};

void testPullsAndClock()
{
    Counter c;
    AudioOutput out(c.pull(), AudioOutput::Backend::Null);
    CHECK(out.ok());
    CHECK(!out.description().empty());
    if (!out.ok())
        return;

    sleepMs(100); // opened, not started: nothing pulled, nothing heard
    CHECK(c.calls == 0);
    CHECK(out.playedFrames() == 0.0);

    out.start();
    sleepMs(300);
    const uint64_t pulled = c.frames;
    const double played = out.playedFrames();
    // Real time: ~300 ms of frames, give or take the device's buffer.
    CHECK(pulled > AudioOutput::kSampleRate / 10);
    CHECK(pulled < AudioOutput::kSampleRate);
    CHECK(played > 0.0);
    CHECK(played <= static_cast<double>(pulled));

    out.stop();
    const uint64_t atStop = c.frames;
    const double playedAtStop = out.playedFrames();
    sleepMs(100); // stopped: no pulls, the clock holds
    CHECK(c.frames == atStop);
    CHECK(out.playedFrames() == playedAtStop);
    CHECK(playedAtStop >= played);
    CHECK(playedAtStop <= static_cast<double>(atStop));

    out.start();
    sleepMs(100);
    CHECK(c.frames > atStop);
    CHECK(out.playedFrames() >= playedAtStop);
    out.stop();
}

void testRapidStartStop()
{
    Counter c;
    AudioOutput out(c.pull(), AudioOutput::Backend::Null);
    if (!out.ok())
        return;
    for (int i = 0; i < 200; ++i) {
        out.start();
        out.stop();
    }
    out.stop(); // already stopped: no-op
    out.start();
    out.start(); // already started: no-op
    const uint64_t before = c.frames;
    sleepMs(100);
    CHECK(c.frames > before); // still pulling after all that
}

void testReopen()
{
    for (int i = 0; i < 50; ++i) {
        Counter c;
        AudioOutput out(c.pull(), AudioOutput::Backend::Null);
        CHECK(out.ok());
        if (i % 2)
            out.start(); // closed while running too
    }
}

} // namespace

int main()
{
    testPullsAndClock();
    testRapidStartStop();
    testReopen();
    if (g_failures == 0)
        std::printf("audio_output_test: all checks passed\n");
    else
        std::printf("audio_output_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
