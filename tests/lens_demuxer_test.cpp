// orbiscore::FisheyeDemux / LensDemuxer with a file per lens (the ONE X2's layout):
// two single-stream videos written here, each frame a flat grey that encodes
// its index, decoded as one lens pair -- in step, after seeks, to the end,
// and in the order the lenses are given; and with read and decode errors made
// on purpose (support/FfmpegFaults.h). Exit code is the failure count.

#include <orbis-core/media/FisheyeDemux.h>
#include <orbis-core/media/StillPicture.h>
#include <orbis-core/media/VulkanVideoDecoder.h>
#include "support/FfmpegFaults.h"
#include "support/TestMedia.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

constexpr int kFps = 25;
constexpr int kFrames = 50;

// Frame n's grey: lens A rises, lens B falls, so neither file's frames can be
// mistaken for the other's.
int greyA(int n) { return 30 + 4 * n; }
int greyB(int n) { return 226 - 4 * n; }

// The frame index whose grey is nearest the decoded picture's mean (green;
// the grey is limited-range luma, which the decode expands).
int frameOf(const orbiscore::Image& img, int (*grey)(int))
{
    double sum = 0;
    for (size_t i = 1; i < img.rgb.size(); i += 3)
        sum += img.rgb[i];
    const double mean = sum / (img.rgb.size() / 3.0);
    int best = -1;
    double bestErr = 1e9;
    for (int n = 0; n < kFrames; ++n) {
        const double rgb = (grey(n) - 16) * 255.0 / 219.0;
        if (std::abs(rgb - mean) < bestErr) {
            bestErr = std::abs(rgb - mean);
            best = n;
        }
    }
    return best;
}

bool writeLens(const std::string& path, int (*grey)(int))
{
    testmedia::VideoSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.fps = kFps;
    spec.frames = kFrames;
    spec.luma = [grey](int frame, int, int) { return static_cast<uint8_t>(grey(frame)); };
    return testmedia::writeVideo(path, spec);
}

// The next pair's frame indices, {-1, -1} at the end.
std::pair<int, int> nextPair(orbiscore::FisheyeDemux& d, int (*grey0)(int), int (*grey1)(int))
{
    orbiscore::Image a, b;
    std::string err;
    if (!d.readNextPair(a, b, nullptr, err)) {
        if (!err.empty())
            std::printf("  readNextPair: %s\n", err.c_str());
        return {-1, -1};
    }
    return {frameOf(a, grey0), frameOf(b, grey1)};
}

void testPairsInStep(const std::string& a, const std::string& b)
{
    std::printf("  two files: every pair in step\n");
    orbiscore::FisheyeDemux d;
    std::string err;
    CHECK(d.open({{a, 0}, {b, 0}}, err));
    CHECK(d.lensCount() == 2);
    int n = 0;
    for (;; ++n) {
        const auto [f0, f1] = nextPair(d, greyA, greyB);
        if (f0 < 0)
            break;
        if (f0 != n || f1 != n) {
            std::printf("  pair %d decoded as %d / %d\n", n, f0, f1);
            CHECK(false);
            return;
        }
    }
    if (n != kFrames)
        std::printf("  %d pairs, not %d\n", n, kFrames);
    CHECK(n == kFrames);
}

void testSeek(const std::string& a, const std::string& b)
{
    std::printf("  two files: seeks land both lenses on the same frame\n");
    orbiscore::FisheyeDemux d;
    std::string err;
    CHECK(d.open({{a, 0}, {b, 0}}, err));
    const auto at = [&](int64_t ms) {
        CHECK(d.seekTo(ms, err));
        return nextPair(d, greyA, greyB);
    };
    CHECK(at(1000) == std::make_pair(25, 25)); // a seek back to a keyframe
    CHECK(at(520) == std::make_pair(13, 13));  // backwards, between keyframes
    CHECK(at(600) == std::make_pair(15, 15));  // a little ahead: decodes on
    CHECK(at(0) == std::make_pair(0, 0));
    CHECK(at(1960) == std::make_pair(49, 49)); // the last frame
    CHECK(nextPair(d, greyA, greyB) == std::make_pair(-1, -1));
}

void testLensOrder(const std::string& a, const std::string& b)
{
    std::printf("  two files: the lenses in the order given\n");
    orbiscore::FisheyeDemux d;
    std::string err;
    CHECK(d.open({{b, 0}, {a, 0}}, err));
    CHECK(d.seekTo(400, err));
    CHECK(nextPair(d, greyB, greyA) == std::make_pair(10, 10));
}

// A camera's proxy: both lenses side by side in one stream (left half lens
// A's grey, right half lens B's), opened as two lenses on its halves -- one
// decoder, each lens its half, in either order.
void testSideBySide(const std::string& dir)
{
    std::printf("  one stream, two lenses side by side: one decode, each lens its half\n");
    const std::string path = dir + "/proxy.mp4";
    testmedia::VideoSpec spec;
    spec.width = 128;
    spec.height = 64;
    spec.fps = kFps;
    spec.frames = kFrames;
    spec.luma = [](int frame, int x, int) {
        return static_cast<uint8_t>(x < 64 ? greyA(frame) : greyB(frame));
    };
    CHECK(testmedia::writeVideo(path, spec));
    const orbiscore::LensRect left{0.0, 0.0, 0.5, 1.0}, right{0.5, 0.0, 0.5, 1.0};
    orbiscore::FisheyeDemux d;
    std::string err;
    CHECK(d.open({{path, 0, right}, {path, 0, left}}, err));
    CHECK(d.lensCount() == 2 && d.lens(1).width == 128);
    orbiscore::Image a, b;
    CHECK(d.seekTo(400, err) && d.readNextPair(a, b, nullptr, err));
    CHECK(a.w == 64 && a.h == 64 && b.w == 64 && b.h == 64);
    // Away from the seam between the halves, which the codec blurs.
    const auto inner = [](const orbiscore::Image& img) {
        orbiscore::Image c;
        c.alloc(img.w - 8, img.h);
        for (int y = 0; y < img.h; ++y)
            for (int x = 4; x < img.w - 4; ++x)
                for (int k = 0; k < 3; ++k)
                    c.rgb[(y * c.w + x - 4) * 3 + k] = img.rgb[(y * img.w + x) * 3 + k];
        return c;
    };
    CHECK(frameOf(inner(a), greyB) == 10 && frameOf(inner(b), greyA) == 10);
    // The shared decoder hands out one frame per pair: on to the end.
    int pairs = 1;
    while (d.readNextPair(a, b, nullptr, err))
        ++pairs;
    CHECK(err.empty() && pairs == kFrames - 10);

    // The decoder's own pair: the same frame for both lenses.
    orbiscore::LensDemuxer demux;
    CHECK(demux.open({{path, 0, left}, {path, 0, right}},
                     [](const auto*, const auto*, auto*, std::string&) { return true; }, err));
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    CHECK(demux.readNextPair(f0, f1, nullptr, err) && f0 && f1 && f0->data[0] == f1->data[0]);
    av_frame_free(&f0);
    av_frame_free(&f1);
}

// At 29.97 fps a frame's time isn't a whole millisecond: `ms` is rounded as
// ever, `us` is the stream's time (what the stabilizer's correction takes).
void testExactTimes(const std::string& dir)
{
    std::printf("  29.97 fps: each pair's time exact to the microsecond\n");
    const std::string a = dir + "/ntsc0.mp4", b = dir + "/ntsc1.mp4";
    for (const std::string& path : {a, b}) {
        testmedia::VideoSpec spec;
        spec.width = 64;
        spec.height = 64;
        spec.fps = 30000;
        spec.fpsDen = 1001;
        spec.frames = 12;
        CHECK(testmedia::writeVideo(path, spec));
    }
    orbiscore::FisheyeDemux d;
    std::string err;
    CHECK(d.open({{a, 0}, {b, 0}}, err));
    orbiscore::Image l0, l1;
    orbiscore::FramePts pts;
    int n = 0;
    for (; d.readNextPair(l0, l1, &pts, err); ++n) {
        const double ms = n * 1001.0 / 30.0;
        CHECK(pts.ms == std::llround(ms));
        CHECK(pts.us == std::llround(ms * 1000.0));
    }
    CHECK(err.empty());
    CHECK(n == 12);
    // And after a seek, between keyframes.
    CHECK(d.seekTo(234, err)); // frame 7, at 233.567 ms
    CHECK(d.readNextPair(l0, l1, &pts, err));
    CHECK(pts.ms == 234);
    CHECK(pts.us == 233567);
}

// 30 fps: frame times aren't whole ms (33.333, 66.667, ...), and reads
// report them rounded. A seek to the ms a read reported finds that frame;
// a ms past it, the next one (a frame a hair under the ms isn't taken for
// it); to the microsecond past a frame's exact time, the next one; to a
// keyframe's time as the index gives it, that keyframe.
void testSeekAtReportedTimes(const std::string& dir)
{
    std::printf("  30 fps: a seek to a reported time finds that frame, a ms past it the next\n");
    const std::string a = dir + "/thirty0.mp4", b = dir + "/thirty1.mp4";
    for (const auto& [path, grey] : {std::pair{a, greyA}, std::pair{b, greyB}}) {
        testmedia::VideoSpec spec;
        spec.width = 64;
        spec.height = 64;
        spec.fps = 30;
        spec.frames = kFrames;
        spec.luma = [grey](int frame, int, int) { return static_cast<uint8_t>(grey(frame)); };
        CHECK(testmedia::writeVideo(path, spec));
    }
    orbiscore::FisheyeDemux d;
    std::string err;
    CHECK(d.open({{a, 0}, {b, 0}}, err));
    std::vector<orbiscore::FramePts> times;
    orbiscore::Image l0, l1;
    orbiscore::FramePts pts;
    while (d.readNextPair(l0, l1, &pts, err))
        times.push_back(pts);
    CHECK(err.empty() && times.size() == size_t(kFrames));
    const auto frameAt = [&](bool ok) {
        orbiscore::Image x, y;
        if (!ok || !d.readNextPair(x, y, nullptr, err))
            return -1;
        return frameOf(x, greyA);
    };
    int wrong = 0;
    for (int n = 0; n < int(times.size()); ++n) {
        CHECK(times[n].ms == orbiscore::MsOfUs(times[n].us));
        wrong += frameAt(d.seekTo(times[n].ms, err)) != n;
        if (n + 1 < int(times.size())) {
            wrong += frameAt(d.seekTo(times[n].ms + 1, err)) != n + 1;
            wrong += frameAt(d.seekToUs(times[n].us + 1, err)) != n + 1;
        }
    }
    if (wrong)
        std::printf("    %d seeks landed on another frame\n", wrong);
    CHECK(wrong == 0);
    // Keyframes every 10 frames: frame 10 at 333.333 ms.
    const int64_t kf = d.nearestKeyframeMs(400);
    CHECK(kf == times[10].ms);
    CHECK(frameAt(d.seekTo(kf, err)) == 10);
}

void testErrors(const std::string& a, const std::string& dir)
{
    std::printf("  two files: errors name the other file\n");
    orbiscore::FisheyeDemux d;
    std::string err;
    const std::string gone = dir + "/gone.mp4";
    CHECK(!d.open({{a, 0}, {gone, 0}}, err));
    CHECK(err.find(gone) != std::string::npos);
    err.clear();
    CHECK(!d.open({{a, 0}, {a, 1}}, err)); // one video stream only
    CHECK(err == "no video stream 1");
}

// Pairs read to the end through any errors (at most a few in a row: a read
// that fails for ever ends it).
struct Run {
    int pairs = 0;
    int errors = 0;
    std::string lastErr;
    bool inStep = true;  // every pair the same frame of both lenses
    bool inOrder = true; // pair n frame n (counting from `first`)
};

Run readThrough(orbiscore::FisheyeDemux& d, int first = 0)
{
    Run run;
    for (int errorsInARow = 0;;) {
        orbiscore::Image a, b;
        std::string err;
        if (d.readNextPair(a, b, nullptr, err)) {
            const int f0 = frameOf(a, greyA), f1 = frameOf(b, greyB);
            run.inStep = run.inStep && f0 == f1;
            run.inOrder = run.inOrder && f0 == first + run.pairs;
            ++run.pairs;
            errorsInARow = 0;
            continue;
        }
        if (err.empty())
            break;
        run.lastErr = err;
        ++run.errors;
        if (++errorsInARow > 3)
            break;
    }
    return run;
}

void testReadErrors(const std::string& a, const std::string& b)
{
    using ffmpegfaults::Call;
    const auto open = [&](orbiscore::FisheyeDemux& d) {
        ffmpegfaults::reset();
        std::string err;
        CHECK(d.open({{a, 0}, {b, 0}}, err));
    };

    std::printf("  a read error: reported, not the end; the next read carries on\n");
    {
        orbiscore::FisheyeDemux d;
        open(d);
        ffmpegfaults::arm(Call::ReadFrame, 30, AVERROR(EIO));
        const Run run = readThrough(d);
        CHECK(run.errors == 1);
        CHECK(run.lastErr.find("Input/output error") != std::string::npos);
        CHECK(run.lastErr.find(a) != std::string::npos || run.lastErr.find(b) != std::string::npos);
        CHECK(run.pairs == kFrames && run.inOrder && run.inStep);
    }

    std::printf("  a decode error: reported, and no frame lost\n");
    {
        orbiscore::FisheyeDemux d;
        open(d);
        ffmpegfaults::arm(Call::ReceiveFrame, 40, AVERROR_INVALIDDATA);
        const Run run = readThrough(d);
        CHECK(run.errors == 1);
        CHECK(run.lastErr.find("decoding lens") != std::string::npos);
        CHECK(run.pairs == kFrames && run.inOrder && run.inStep);
    }

    std::printf("  a packet the decoder refuses: reported, then on to the end\n");
    {
        orbiscore::FisheyeDemux d;
        open(d);
        ffmpegfaults::arm(Call::SendPacket, 30, AVERROR_INVALIDDATA);
        const Run run = readThrough(d);
        CHECK(run.errors == 1);
        CHECK(run.lastErr.find("decoding lens") != std::string::npos);
        // The refused packet's frame is gone (and the lenses out of step
        // after it): only that the read gets to the end is checked.
        CHECK(run.pairs >= kFrames - 2);
    }

    std::printf("  a refused flush: reported, and the frames held still come out\n");
    {
        orbiscore::FisheyeDemux d;
        open(d);
        ffmpegfaults::arm(Call::SendFlush, 0, AVERROR(ENOMEM));
        const Run run = readThrough(d);
        CHECK(run.errors == 1);
        CHECK(run.pairs == kFrames && run.inOrder && run.inStep);
    }

    std::printf("  a decoder that stalls after the flush: an error, not a hang\n");
    {
        orbiscore::FisheyeDemux d;
        open(d);
        ffmpegfaults::arm(Call::ReceiveDraining, 0, AVERROR(EAGAIN), -1);
        const Run stalled = readThrough(d);
        CHECK(stalled.errors > 0);
        CHECK(stalled.lastErr.find("stalled") != std::string::npos);
        CHECK(stalled.inOrder && stalled.inStep);
        ffmpegfaults::reset();
        const Run rest = readThrough(d, stalled.pairs);
        CHECK(rest.errors == 0 && rest.inOrder && rest.inStep);
        CHECK(stalled.pairs + rest.pairs == kFrames);
    }
    ffmpegfaults::reset();
}

// The frames of the next pair, {-1, -1} when it fails.
std::pair<int, int> pairAt(orbiscore::FisheyeDemux& d, int64_t* ms = nullptr)
{
    orbiscore::Image l0, l1;
    orbiscore::FramePts pts;
    std::string err;
    if (!d.readNextPair(l0, l1, &pts, err))
        return {-1, -1};
    if (ms)
        *ms = pts.ms;
    return {frameOf(l0, greyA), frameOf(l1, greyB)};
}

// A seek that moves one lens's file and not the other's: no read until a
// seek works, and none from where the reads were, nor half from each file.
void testFailedSeek(const std::string& a, const std::string& b, const std::string& dir)
{
    using ffmpegfaults::Call;
    std::printf("  a seek failing for the second file: no pair, then the seek's frame\n");
    {
        orbiscore::FisheyeDemux d;
        ffmpegfaults::reset();
        std::string err;
        CHECK(d.open({{a, 0}, {b, 0}}, err));
        for (int i = 0; i < 5; ++i)
            CHECK(pairAt(d).first == i);
        ffmpegfaults::arm(Call::SeekFrame, 1, AVERROR(EIO)); // the first file's works
        CHECK(!d.seekTo(1000, err));
        CHECK(err.find("Input/output error") != std::string::npos && err.find(b) != std::string::npos);
        orbiscore::Image l0, l1;
        std::string readErr;
        CHECK(!d.readNextPair(l0, l1, nullptr, readErr) && readErr == err);
        ffmpegfaults::reset();
        readErr.clear();
        CHECK(!d.readNextPair(l0, l1, nullptr, readErr) && readErr == err); // until a seek works
        err.clear();
        CHECK(d.seekTo(1000, err));
        int64_t ms = -1;
        CHECK(pairAt(d, &ms) == std::make_pair(25, 25) && ms == 1000);
        // As a fresh seek has it.
        orbiscore::FisheyeDemux fresh;
        CHECK(fresh.open({{a, 0}, {b, 0}}, err) && fresh.seekTo(1000, err));
        CHECK(pairAt(fresh) == std::make_pair(25, 25));
        CHECK(pairAt(d) == std::make_pair(26, 26));
    }

    std::printf("  the next part's file failing to rewind: an error, then on into the part\n");
    {
        const std::string a2 = dir + "/part2_lens0.mp4", b2 = dir + "/part2_lens1.mp4";
        std::filesystem::copy_file(a, a2, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(b, b2, std::filesystem::copy_options::overwrite_existing);
        const int64_t partUs = int64_t(kFrames) * 1000000 / kFps;
        orbiscore::FisheyeDemux d;
        ffmpegfaults::reset();
        std::string err;
        CHECK(d.open(std::vector<orbiscore::ClipPart>{{0, {{a, 0}, {b, 0}}}, {partUs, {{a2, 0}, {b2, 0}}}},
                     err));
        for (int i = 0; i < kFrames; ++i)
            CHECK(pairAt(d).first == i);
        ffmpegfaults::arm(Call::SeekFrame, 1, AVERROR(EIO)); // the second part's second file
        orbiscore::Image l0, l1;
        CHECK(!d.readNextPair(l0, l1, nullptr, err));
        CHECK(err.find("Input/output error") != std::string::npos && err.find(b2) != std::string::npos);
        int pairs = 0;
        bool inOrder = true;
        for (int64_t ms = -1;; ++pairs) {
            const auto [f0, f1] = pairAt(d, &ms);
            if (f0 < 0)
                break;
            inOrder = inOrder && f0 == pairs && f1 == pairs && ms == partUs / 1000 + pairs * 1000 / kFps;
        }
        CHECK(inOrder && pairs == kFrames);
        std::filesystem::remove(a2);
        std::filesystem::remove(b2);
    }
    ffmpegfaults::reset();
}

void testAllocErrors(const std::string& a, const std::string& b, const std::string& dir)
{
    using ffmpegfaults::Call;
    // The demux fails to open with `want` in the error, and the same demux
    // then opens and reads the clip to its end.
    const auto failsOpen = [&](Call call, int after, int rc, const std::string& want) {
        orbiscore::FisheyeDemux d;
        ffmpegfaults::reset();
        ffmpegfaults::arm(call, after, rc);
        std::string err;
        CHECK(!d.open({{a, 0}, {b, 0}}, err));
        CHECK(ffmpegfaults::failed(call) == 1);
        CHECK(err.find(want) != std::string::npos);
        ffmpegfaults::reset();
        err.clear();
        CHECK(d.open({{a, 0}, {b, 0}}, err));
        const Run run = readThrough(d);
        CHECK(run.errors == 0 && run.pairs == kFrames && run.inOrder && run.inStep);
    };
    std::printf("  no memory for a decoder, its parameters or the packet: open fails\n");
    failsOpen(Call::AllocContext, 0, 0, "out of memory for the decoder of video stream 0");
    failsOpen(Call::AllocContext, 1, 0, "out of memory for the decoder of video stream 0: " + b);
    failsOpen(Call::ParamsToContext, 1, AVERROR(ENOMEM), "setting up the decoder of video stream");
    failsOpen(Call::PacketAlloc, 0, 0, "out of memory");

    std::printf("  no memory for a decoded frame: reported, and no frame lost\n");
    {
        orbiscore::FisheyeDemux d;
        ffmpegfaults::reset();
        std::string err;
        CHECK(d.open({{a, 0}, {b, 0}}, err));
        ffmpegfaults::arm(Call::FrameAlloc, 20, 0);
        const Run run = readThrough(d);
        CHECK(ffmpegfaults::failed(Call::FrameAlloc) == 1);
        CHECK(run.errors == 1);
        CHECK(run.lastErr.find("out of memory decoding lens") != std::string::npos);
        CHECK(run.pairs == kFrames && run.inOrder && run.inStep);
    }
    ffmpegfaults::reset();

    std::printf("  a pair that doesn't convert: an error, no picture, then the next pair\n");
    for (Call call : {Call::Scale, Call::ScalerSetup}) {
        orbiscore::FisheyeDemux d;
        ffmpegfaults::reset();
        std::string err;
        CHECK(d.open({{a, 0}, {b, 0}}, err));
        orbiscore::Image l0, l1;
        CHECK(d.readNextPair(l0, l1, nullptr, err));
        ffmpegfaults::arm(call, 0, AVERROR(ENOMEM));
        CHECK(!d.readNextPair(l0, l1, nullptr, err));
        CHECK(ffmpegfaults::failed(call) == 1);
        CHECK(err.find("RGB conversion") != std::string::npos ||
              err.find("to RGB failed") != std::string::npos);
        CHECK(l0.empty() && l1.empty());
        const Run rest = readThrough(d, 2); // pair 1 is gone
        CHECK(rest.errors == 0 && rest.inOrder && rest.inStep && rest.pairs == kFrames - 2);
    }

    std::printf("  a preview picture that doesn't convert: no picture\n");
    if (const std::vector<uint8_t> pic = testmedia::encodeH264Picture(64, 64, 128); !pic.empty()) {
        for (Call call : {Call::Scale, Call::ScalerSetup}) {
            ffmpegfaults::reset();
            ffmpegfaults::arm(call, 0, AVERROR(ENOMEM));
            orbiscore::Image img;
            std::string err;
            CHECK(!orbiscore::DecodeStillPicture(pic, "h264", 64, img, err));
            CHECK(!err.empty() && img.empty());
            ffmpegfaults::reset();
            CHECK(orbiscore::DecodeStillPicture(pic, "h264", 64, img, err) && img.w == 64);
        }
    }
    ffmpegfaults::reset();

    // The Vulkan decoder's per-lens device reference; needs a GPU that
    // decodes H.264.
    AVBufferRef* dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) < 0) {
        std::printf("  (no Vulkan device: the decoder's device reference not checked)\n");
        return;
    }
    const std::string clip = dir + "/h264_two_lenses.mp4";
    testmedia::VideoSpec spec;
    spec.width = 256;
    spec.height = 256;
    spec.frames = 10;
    spec.streams = 2;
    spec.h264 = true;
    std::string err;
    if (!testmedia::writeVideo(clip, spec) || !orbiscore::VulkanVideoDecoder().open(dev, clip, err)) {
        std::printf("  (no H.264 encoder or Vulkan H.264 decode: the device reference not checked)\n");
        av_buffer_unref(&dev);
        return;
    }
    std::printf("  no device reference for a lens's decoder: open fails, references balanced\n");
    for (int after : {0, 1, 2}) { // the decoder's own, lens 0's, lens 1's
        orbiscore::VulkanVideoDecoder v;
        ffmpegfaults::arm(Call::BufferRef, after, 0);
        err.clear();
        CHECK(!v.open(dev, clip, err));
        CHECK(ffmpegfaults::failed(Call::BufferRef) == 1);
        CHECK(err.find("out of memory") != std::string::npos);
        CHECK(!v.unsupported());
        ffmpegfaults::reset();
        CHECK(av_buffer_get_ref_count(dev) == 1);
        err.clear();
        CHECK(v.open(dev, clip, err));
    }
    av_buffer_unref(&dev);
}

} // namespace

int main()
{
    const auto dir = std::filesystem::temp_directory_path() / "orbis_lens_demuxer_test";
    std::filesystem::create_directories(dir);
    const std::string a = (dir / "lens0.mp4").string();
    const std::string b = (dir / "lens1.mp4").string();
    if (!writeLens(a, greyA) || !writeLens(b, greyB)) {
        std::printf("FAIL  couldn't write the test videos\n");
        return 1;
    }
    testPairsInStep(a, b);
    testSeek(a, b);
    testLensOrder(a, b);
    testErrors(a, dir.string());
    testReadErrors(a, b);
    testAllocErrors(a, b, dir.string());
    testFailedSeek(a, b, dir.string());
    testExactTimes(dir.string());
    testSeekAtReportedTimes(dir.string());
    testSideBySide(dir.string());
    if (!g_failures)
        std::filesystem::remove_all(dir);
    std::printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures;
}
