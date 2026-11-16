// The playback controller's measure of a decoder that can't keep up
// (DecodePace): reads slower than the clip over a whole window, not a slow
// moment, and not the first read after a seek. Exit code is the failure
// count.

#include "DecodePace.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// Feeds `seconds` of a 30 fps clip whose reads take `readMs`, from `startS`;
// true when any read said the decoder is behind.
bool play(DecodePace &pace, double readMs, double seconds, double startS = 0.0)
{
    const double frameMs = 1000.0 / 30.0;
    bool behind = false;
    for (double t = startS; t < startS + seconds; t += frameMs / 1000.0)
        behind = pace.add(readMs, frameMs, t) || behind;
    return behind;
}

} // namespace

int main()
{
    {
        DecodePace pace;
        CHECK(!play(pace, 30.0, 20.0)); // keeps up: never
    }
    {
        DecodePace pace;
        CHECK(play(pace, 45.0, 6.0)); // a third slower all along
    }
    {
        DecodePace pace;
        CHECK(!play(pace, 45.0, 4.0)); // too short a stretch to say
    }
    {
        // A slow second inside a fast window: no.
        DecodePace pace;
        bool behind = play(pace, 20.0, 2.0);
        behind = play(pace, 60.0, 1.0, 2.0) || behind;
        behind = play(pace, 20.0, 3.0, 3.0) || behind;
        CHECK(!behind);
    }
    {
        // The first read after a reset (the seek's decode) doesn't count.
        DecodePace pace;
        pace.reset();
        CHECK(!pace.add(5000.0, 33.3, 0.0));
        CHECK(!play(pace, 30.0, 6.0, 0.01));
    }
    {
        // A reset (a seek, a pause) starts the window afresh.
        DecodePace pace;
        CHECK(!play(pace, 45.0, 4.0));
        pace.reset();
        CHECK(!play(pace, 45.0, 4.0, 4.0));
        CHECK(play(pace, 45.0, 2.0, 8.0));
    }
    if (g_failures == 0)
        std::printf("decode_pace_test: all checks passed\n");
    else
        std::printf("decode_pace_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
