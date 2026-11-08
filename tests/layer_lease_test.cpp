// src/ui/LayerLease: a crossfade's layer reading playback's own source of
// its clip, then handing it back -- the frames in order, none twice, none
// lost, the source left at the clip's first frame or the hand-back saying it
// isn't. On BlankClipSource (frames at the clip's rate, no GPU).

#include "BlankClipSource.h"
#include "LayerLease.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

constexpr double kFps = 30.0; // frames at 0, 33, 67, 100, ... 967, 1000

std::shared_ptr<stitch::IStitchSource> source(int64_t seekMs)
{
    auto src = std::make_shared<BlankClipSource>(kFps, 60000, false);
    src->Open("clip.insv", {});
    src->SeekTo(seekMs);
    return src;
}

std::atomic<int> g_reads{0};

LayerLease::Render render()
{
    return [](stitch::IStitchSource &src, exportpipe::VideoFrame &out) {
        const stitch::Frame f = src.ReadFrame();
        if (f.rgba.empty())
            return false;
        ++g_reads;
        out.ptsMs = f.timestamp_ms;
        return true;
    };
}

// The layer reads all its frames itself; the source is left at the clip's
// first frame.
void testLayerReadsToItsEnd()
{
    int answered = 0;
    auto lease = std::make_shared<LayerLease>(7, 500, 1000, true, render(), [&] { ++answered; });
    exportpipe::VideoFrame f;
    CHECK(lease->next(f) == LayerLease::Next::Waiting);
    CHECK(lease->lend(source(500)));
    CHECK(answered == 1);
    std::vector<int64_t> pts;
    while (lease->next(f) == LayerLease::Next::Frame)
        pts.push_back(f.ptsMs);
    CHECK(!pts.empty() && pts.front() == 500 && pts.back() == 967);
    CHECK(pts.size() == 15);
    bool atEnd = false;
    auto src = lease->takeBack(true, atEnd);
    CHECK(src && atEnd);
    CHECK(src->ReadFrame().timestamp_ms == 1000); // playback's first, no seek
    CHECK(lease->next(f) == LayerLease::Next::End);
}

// Playback takes it back with the layer behind: the rest is read for the
// layer first, which goes on from that buffer.
void testTakeBackReadsTheRest()
{
    auto lease = std::make_shared<LayerLease>(7, 500, 1000, true, render(), nullptr);
    lease->lend(source(500));
    exportpipe::VideoFrame f;
    CHECK(lease->next(f) == LayerLease::Next::Frame && f.ptsMs == 500);
    CHECK(lease->next(f) == LayerLease::Next::Frame && f.ptsMs == 533);
    bool atEnd = false;
    auto src = lease->takeBack(true, atEnd);
    CHECK(atEnd);
    CHECK(src->ReadFrame().timestamp_ms == 1000);
    std::vector<int64_t> pts;
    while (lease->next(f) == LayerLease::Next::Frame)
        pts.push_back(f.ptsMs);
    CHECK(pts.size() == 13 && pts.front() == 567 && pts.back() == 967);
    CHECK(lease->next(f) == LayerLease::Next::End);
}

// Too much left to read at the cut (a long crossfade the layer fell behind
// on): a few frames for the layer, then it reads its own; playback seeks.
void testTakeBackCapped()
{
    auto lease = std::make_shared<LayerLease>(7, 0, 2000, true, render(), nullptr);
    lease->lend(source(0));
    bool atEnd = true;
    lease->takeBack(true, atEnd);
    CHECK(!atEnd);
    exportpipe::VideoFrame f;
    size_t n = 0;
    LayerLease::Next r;
    while ((r = lease->next(f)) == LayerLease::Next::Frame)
        ++n;
    CHECK(n == LayerLease::kMaxBuffered);
    CHECK(r == LayerLease::Next::Gone);
}

// The trim-in on a frame (rounded to the ms): that frame is playback's; a ms
// later it's the layer's last.
void testEndOnAFrame()
{
    for (const auto [endMs, layerLast, first] :
         {std::tuple<int64_t, int64_t, int64_t>{967, 933, 967}, {968, 967, 1000}}) {
        auto lease = std::make_shared<LayerLease>(7, 800, endMs, true, render(), nullptr); // a frame at 967
        lease->lend(source(800));
        exportpipe::VideoFrame f;
        int64_t last = -1;
        while (lease->next(f) == LayerLease::Next::Frame)
            last = f.ptsMs;
        CHECK(last == layerLast);
        bool atEnd = false;
        auto src = lease->takeBack(true, atEnd);
        CHECK(atEnd && src->ReadFrame().timestamp_ms == first);
    }
}

// Declined, or taken back before the layer read: it reads its own; a lease
// the layer left can't be lent.
void testDeclinedAndLeft()
{
    int answered = 0;
    auto lease = std::make_shared<LayerLease>(7, 500, 1000, true, render(), [&] { ++answered; });
    lease->decline();
    CHECK(answered == 1);
    exportpipe::VideoFrame f;
    CHECK(lease->next(f) == LayerLease::Next::Gone);
    CHECK(!lease->lend(source(500)));

    auto left = std::make_shared<LayerLease>(7, 500, 1000, true, render(), nullptr);
    left->leave();
    CHECK(!left->lend(source(500)));

    auto revoked = std::make_shared<LayerLease>(7, 500, 1000, true, render(), nullptr);
    revoked->lend(source(500));
    bool atEnd = true;
    CHECK(revoked->takeBack(false, atEnd) && !atEnd); // a seek: nothing read for it
    CHECK(revoked->next(f) == LayerLease::Next::Gone);

    // Left half-way: playback gets it back, not at the clip's start.
    auto half = std::make_shared<LayerLease>(7, 500, 1000, true, render(), nullptr);
    half->lend(source(500));
    half->next(f);
    half->leave();
    const int before = g_reads;
    CHECK(half->takeBack(true, atEnd) && !atEnd);
    CHECK(g_reads == before); // nothing read for a layer that's gone
}

// The layer reading on its thread while playback takes it back: every frame
// once, in order, the layer's before playback's.
void testThreads()
{
    for (int run = 0; run < 50; ++run) {
        auto lease = std::make_shared<LayerLease>(7, 0, 3000, true, render(), nullptr);
        lease->lend(source(0));
        std::vector<int64_t> layer;
        std::thread t([&] {
            exportpipe::VideoFrame f;
            LayerLease::Next r;
            while ((r = lease->next(f)) == LayerLease::Next::Frame)
                layer.push_back(f.ptsMs);
        });
        std::this_thread::yield();
        bool atEnd = false;
        auto src = lease->takeBack(true, atEnd);
        t.join();
        bool ordered = true;
        for (size_t i = 1; i < layer.size(); ++i)
            ordered &= layer[i] > layer[i - 1];
        CHECK(ordered);
        if (atEnd) {
            CHECK(!layer.empty() && layer.back() < 3000);
            CHECK(src->ReadFrame().timestamp_ms == 3000);
        }
    }
}

// The second half: playback has read the clip up to its trim-out (2000) and
// lends the source there; the layer reads to its end, then it's done.
void testPastTrimOut()
{
    auto src = source(1900);
    while (src->ReadFrame().timestamp_ms < 1967) {
    } // playback's last frame: 1967, the next one at 2000
    auto lease = std::make_shared<LayerLease>(7, 1983, 2500, false, render(), nullptr);
    CHECK(!lease->done());
    CHECK(lease->lend(src));
    CHECK(!lease->done());
    exportpipe::VideoFrame f;
    std::vector<int64_t> pts;
    while (lease->next(f) == LayerLease::Next::Frame)
        pts.push_back(f.ptsMs);
    CHECK(pts.size() == 15 && pts.front() == 2000 && pts.back() == 2467);
    CHECK(lease->done());
    bool atEnd = false;
    CHECK(lease->takeBack(false, atEnd) == src && atEnd);
    CHECK(lease->next(f) == LayerLease::Next::End);

    // Left half-way (a seek): done too.
    auto left = std::make_shared<LayerLease>(7, 0, 1000, false, render(), nullptr);
    left->lend(source(0));
    left->next(f);
    CHECK(!left->done());
    left->leave();
    CHECK(left->done());
}

// The lender: an ask wakes playback, which takes each once.
void testLender()
{
    int wakes = 0, answered = 0;
    LayerLender lender([&] { ++wakes; }, [&] { ++answered; });
    CHECK(!lender.anyAsks());
    auto a = lender.ask(3, 100, 900, true, render());
    CHECK(wakes == 1 && lender.anyAsks());
    auto asks = lender.takeAsks();
    CHECK(asks.size() == 1 && asks[0] == a && !lender.anyAsks());
    CHECK(a->clipId() == 3 && a->fromMs() == 100 && a->endMs() == 900);
    a->decline();
    CHECK(answered == 1);
}

} // namespace

int main()
{
    testLayerReadsToItsEnd();
    testTakeBackReadsTheRest();
    testTakeBackCapped();
    testEndOnAFrame();
    testDeclinedAndLeft();
    testThreads();
    testPastTrimOut();
    testLender();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("layer_lease_test: all passed\n");
    return 0;
}
