// orbiscore::FisheyeDemux / LensDemuxer with a file per lens (the ONE X2's layout):
// two single-stream videos written here, each frame a flat grey that encodes
// its index, decoded as one lens pair -- in step, after seeks, to the end,
// in the order the lenses are given, and paired by time when one lens lacks a
// frame; and with read and decode errors made on purpose
// (support/FfmpegFaults.h). Also what the Vulkan decoder hands a frame's
// consumer and writes back into the frame after it. Exit code is the failure
// count.

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
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/mem.h>
}

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
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
// A lens without a frame the other has -- lens 1 lacks frame 9, lens 0 frame
// 19, each the frame before a keyframe so the rest decodes as written: that
// frame is left out and every other pair is one moment, in content and in
// time, read in order and after seeks at and around the gaps. With a file
// per lens and with both lenses in one file.
void testMissingFrames(const std::string& dir)
{
    std::printf("  a frame missing from one lens: left out, every other pair in step\n");
    const auto keep = [](int lens, int frame) {
        return !(lens == 1 && frame == 9) && !(lens == 0 && frame == 19);
    };
    const std::string a = dir + "/gap0.mp4", b = dir + "/gap1.mp4", both = dir + "/gap2.mp4";
    const auto write = [&](const std::string& path, int streams, int lens, int (*grey)(int)) {
        testmedia::VideoSpec spec;
        spec.width = 64;
        spec.height = 64;
        spec.fps = kFps;
        spec.frames = kFrames;
        spec.streams = streams;
        spec.keep = [=](int stream, int frame) { return keep(streams == 1 ? lens : stream, frame); };
        spec.luma = [grey](int frame, int, int) { return static_cast<uint8_t>(grey(frame)); };
        return testmedia::writeVideo(path, spec);
    };
    CHECK(write(a, 1, 0, greyA));
    CHECK(write(b, 1, 1, greyB));
    CHECK(write(both, 2, 0, greyA)); // the same picture in both streams

    struct Layout {
        const char* name;
        std::vector<orbiscore::LensSource> lenses;
        int (*grey1)(int);
    };
    const Layout layouts[] = {{"a file per lens", {{a, 0}, {b, 0}}, greyB},
                              {"one file", {{both, 0}, {both, 1}}, greyA}};
    for (const Layout& l : layouts) {
        orbiscore::FisheyeDemux d;
        std::string err;
        CHECK(d.open(l.lenses, err));
        CHECK(d.lensCount() == 2);
        // The next pair's frames, and whether its time is lens 0's frame's.
        const auto next = [&] {
            orbiscore::Image x, y;
            orbiscore::FramePts pts;
            if (!d.readNextPair(x, y, &pts, err))
                return std::make_pair(-1, -1);
            const int f0 = frameOf(x, greyA);
            if (pts.us != f0 * 1000000LL / kFps)
                return std::make_pair(f0, -2);
            return std::make_pair(f0, frameOf(y, l.grey1));
        };
        int wrong = 0, n = 0;
        for (int want = 0; want < kFrames; ++want) {
            if (want == 9 || want == 19)
                continue;
            const auto got = next();
            if (got != std::make_pair(want, want)) {
                std::printf("    %s: pair %d read as %d / %d\n", l.name, want, got.first, got.second);
                ++wrong;
            }
            ++n;
        }
        CHECK(wrong == 0);
        CHECK(next() == std::make_pair(-1, -1));
        CHECK(err.empty());
        const auto at = [&](int64_t ms) {
            CHECK(d.seekTo(ms, err));
            return next();
        };
        CHECK(at(320) == std::make_pair(8, 8)); // before the gap, then across it
        CHECK(next() == std::make_pair(10, 10));
        CHECK(at(360) == std::make_pair(10, 10)); // at lens 1's missing frame
        CHECK(at(720) == std::make_pair(18, 18));
        CHECK(next() == std::make_pair(20, 20));
        CHECK(at(760) == std::make_pair(20, 20)); // at lens 0's
        CHECK(at(0) == std::make_pair(0, 0));
    }
}

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

// What a decoded pair holds: a picture per stream (both lenses in one stream
// share theirs), each in the stream's own format -- 10-bit samples in two
// bytes, as the GPU's P010.
void testPairPictureBytes(const std::string& a, const std::string& b, const std::string& dir)
{
    std::printf("  a pair's picture bytes: per stream, at the stream's bit depth\n");
    const auto setup = [](const auto*, const auto*, auto*, std::string&) { return true; };
    std::string err;
    orbiscore::LensDemuxer twoFiles;
    CHECK(twoFiles.open({{a, 0, {}}, {b, 0, {}}}, setup, err));
    CHECK(twoFiles.lens(0).pictureBytes == 64 * 64 * 3 / 2);
    CHECK(twoFiles.pairPictureBytes() == 2 * 64 * 64 * 3 / 2);

    const std::string packed = dir + "/packed.mp4";
    testmedia::VideoSpec spec;
    spec.width = 128;
    spec.height = 64;
    spec.frames = 10;
    CHECK(testmedia::writeVideo(packed, spec));
    const orbiscore::LensRect left{0.0, 0.0, 0.5, 1.0}, right{0.5, 0.0, 0.5, 1.0};
    orbiscore::LensDemuxer oneStream;
    CHECK(oneStream.open({{packed, 0, left}, {packed, 0, right}}, setup, err));
    CHECK(oneStream.pairPictureBytes() == 128 * 64 * 3 / 2);

    const std::string deep = dir + "/main10.mkv";
    spec.width = 64;
    spec.streams = 2;
    spec.tenBit = true;
    CHECK(testmedia::writeVideo(deep, spec));
    orbiscore::LensDemuxer tenBit;
    CHECK(tenBit.open(deep, setup, err));
    CHECK(tenBit.lensCount() == 2 && tenBit.lens(1).bitDepth == 10);
    CHECK(tenBit.pairPictureBytes() == 2 * 64 * 64 * 3);
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
    using Lens = std::pair<std::string, int (*)(int)>;
    for (const auto& [path, grey] : {Lens{a, greyA}, Lens{b, greyB}}) {
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
        // The refused packet's frame is gone, and with it that pair; the
        // lenses stay in step past it.
        CHECK(run.pairs == kFrames - 1);
        CHECK(run.inStep);
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

// The synchronisation state a decoded frame's consumer gets from lockPair
// and gives back through unlockPair, as FFmpeg's next barrier on the image
// reads it: the layout, last access and timeline value the consumer's work
// left -- a compute read, a transfer copy -- and the frame's own three when
// it did no GPU work. Both lenses in one packed frame: written once, by
// lens 0's result. Frames made up here, no device.
void testFrameState()
{
    std::printf("  a frame's layout, access and timeline value through lock and unlock\n");
    AVVulkanFramesContext vfc{};
    vfc.format[0] = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    AVHWFramesContext fc{};
    fc.hwctx = &vfc;
    AVBufferRef fcRef{};
    fcRef.data = reinterpret_cast<uint8_t*>(&fc);
    AVVkFrame* vk[2] = {av_vk_frame_alloc(), av_vk_frame_alloc()};
    AVFrame frame[2]{};
    for (int i = 0; i < 2; ++i) {
        vk[i]->layout[0] = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        // The field's type is FFmpeg's (32-bit VkAccessFlagBits in 7.1).
        vk[i]->access[0] = static_cast<std::remove_reference_t<decltype(vk[i]->access[0])>>(
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vk[i]->sem_value[0] = 5;
        frame[i].hw_frames_ctx = &fcRef;
        frame[i].data[0] = reinterpret_cast<uint8_t*>(vk[i]);
    }
    orbiscore::VulkanVideoDecoder dec;
    orbiscore::VulkanLensFrame l0, l1;
    const auto stateIs = [&](int i, uint64_t value, VkImageLayout layout, VkAccessFlags2 access) {
        return vk[i]->sem_value[0] == value && vk[i]->layout[0] == layout &&
               static_cast<VkAccessFlags2>(vk[i]->access[0]) == access;
    };

    dec.lockPair(&frame[0], &frame[1], l0, l1);
    CHECK(l0.semWaitValue == 5 && l0.layout == VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR &&
          l0.access == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(l1.image == vk[1]->img[0] && l1.access == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // A compute read (the stitch).
    dec.unlockPair(&frame[0], &frame[1],
                   {6, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_READ_BIT},
                   {6, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_READ_BIT});
    for (int i = 0; i < 2; ++i)
        CHECK(stateIs(i, 6, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_READ_BIT));
    // A transfer copy (the viewport's lens fill).
    dec.lockPair(&frame[0], &frame[1], l0, l1);
    CHECK(l0.semWaitValue == 6 && l0.access == VK_ACCESS_2_SHADER_READ_BIT);
    dec.unlockPair(&frame[0], &frame[1],
                   {7, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT},
                   {7, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT});
    for (int i = 0; i < 2; ++i)
        CHECK(stateIs(i, 7, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT));
    // No GPU work: the frames as they were.
    dec.lockPair(&frame[0], &frame[1], l0, l1);
    dec.unlockPair(&frame[0], &frame[1], {l0.semWaitValue, l0.layout, l0.access},
                   {l1.semWaitValue, l1.layout, l1.access});
    for (int i = 0; i < 2; ++i)
        CHECK(stateIs(i, 7, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT));

    // Both lenses in one frame (a camera's proxy): one image, one state.
    frame[1].data[0] = frame[0].data[0];
    dec.lockPair(&frame[0], &frame[1], l0, l1);
    CHECK(l1.image == l0.image && l1.semWaitValue == 7 &&
          l1.access == VK_ACCESS_2_TRANSFER_READ_BIT);
    dec.unlockPair(&frame[0], &frame[1],
                   {8, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_READ_BIT},
                   {8, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_READ_BIT});
    CHECK(stateIs(0, 8, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_READ_BIT));
    CHECK(stateIs(1, 7, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT));
    for (AVVkFrame* f : vk)
        av_free(f);
}

// A series of photos (an Insta360 Interval series: JPEGs, both lenses side
// by side), one a part: each photo at its part's start to the microsecond,
// whatever the image demuxer's own rate (25 a second for jpeg_pipe); on through all of them, seeks into
// any, with only a few files open at once; a photo gone or unlike the first
// fails when it is reached, as open() would, and the rest still reads.
void testPhotoSeries(const std::string& dir)
{
    std::printf("  a series of photos, one a part: their times, seeks, few files open\n");
    constexpr int kPhotos = 20;
    // 30 a second: each shows to the next one's start, 33333 or 33334 µs.
    const auto startUs = [](int i) { return int64_t(std::llround(i * 1e6 / 30.0)); };
    const orbiscore::LensRect left{0.0, 0.0, 0.5, 1.0}, right{0.5, 0.0, 0.5, 1.0};
    std::vector<std::string> paths;
    std::vector<orbiscore::ClipPart> parts;
    for (int i = 0; i < kPhotos; ++i) {
        const std::string path = dir + "/photo_" + std::to_string(i) + ".insp";
        if (!testmedia::writeJpeg(path, 128, 64, [i](int x, int) {
                return static_cast<uint8_t>(x < 64 ? greyA(i) : greyB(i));
            })) {
            std::printf("  (no MJPEG encoder: skipped)\n");
            return;
        }
        paths.push_back(path);
        parts.push_back({startUs(i), {{path, 0, left}, {path, 0, right}}, startUs(i + 1) - startUs(i)});
    }
    // Full-range JPEG greys come back as they were; away from the halves' seam.
    const auto photoOf = [](const orbiscore::Image& img, int (*grey)(int)) {
        double sum = 0;
        int n = 0;
        for (int y = 0; y < img.h; ++y)
            for (int x = 4; x < img.w - 4; ++x, ++n)
                sum += img.rgb[(y * img.w + x) * 3 + 1];
        int best = -1;
        for (int i = 0; i < kPhotos; ++i)
            if (best < 0 || std::abs(grey(i) - sum / n) < std::abs(grey(best) - sum / n))
                best = i;
        return best;
    };
    const auto next = [&](orbiscore::FisheyeDemux& d, int64_t* us = nullptr) {
        orbiscore::Image a, b;
        orbiscore::FramePts pts;
        std::string err;
        if (!d.readNextPair(a, b, &pts, err))
            return err.empty() ? -1 : -2;
        if (us)
            *us = pts.us;
        const int i = photoOf(a, greyA);
        return photoOf(b, greyB) == i ? i : -3;
    };

    orbiscore::FisheyeDemux d;
    std::string err;
    CHECK(d.open(parts, err));
    CHECK(std::abs(d.lens(0).fps - 30.0) < 1e-3 && d.lens(0).width == 128); // to the µs
    CHECK(d.lens(0).durationUs == startUs(kPhotos));
    bool inOrder = true;
    int photos = 0;
    for (int64_t us = -1;; ++photos) {
        const int i = next(d, &us);
        if (i < 0) {
            CHECK(i == -1);
            break;
        }
        inOrder = inOrder && i == photos && us == startUs(i);
    }
    CHECK(inOrder && photos == kPhotos);
    int64_t us = -1;
    CHECK(d.seekTo(orbiscore::MsOfUs(startUs(13)), err) && next(d, &us) == 13 && us == startUs(13));
    CHECK(next(d) == 14);
    CHECK(d.seekTo(orbiscore::MsOfUs(startUs(2)), err) && next(d) == 2);
    CHECK(d.seekTo(orbiscore::MsOfUs(startUs(7)) + 5, err) && next(d) == 8); // after 7's time
    CHECK(d.nearestKeyframeMs(orbiscore::MsOfUs(startUs(7)) + 5) == orbiscore::MsOfUs(startUs(7)));
    CHECK(d.nearestKeyframeMs(orbiscore::MsOfUs(startUs(7)) + 25) == orbiscore::MsOfUs(startUs(8)));

    std::printf("  ... no more files open than kMaxOpenFiles, through all and back\n");
    {
        orbiscore::LensDemuxer demux;
        CHECK(demux.open(parts, [](const auto*, const auto*, auto*, std::string&) { return true; },
                         err));
        CHECK(demux.openFileCount() == 2); // the first and the last
        int most = 0, read = 0;
        AVFrame* f0 = nullptr;
        AVFrame* f1 = nullptr;
        while (demux.readNextPair(f0, f1, nullptr, err)) {
            ++read;
            most = std::max(most, demux.openFileCount());
            av_frame_free(&f0);
            av_frame_free(&f1);
        }
        CHECK(err.empty() && read == kPhotos && most == orbiscore::LensDemuxer::kMaxOpenFiles);
        CHECK(demux.seekTo(0, err) && demux.readNextPair(f0, f1, nullptr, err));
        av_frame_free(&f0);
        av_frame_free(&f1);
        CHECK(demux.openFileCount() <= orbiscore::LensDemuxer::kMaxOpenFiles);
    }

    std::printf("  ... a photo gone since open: an error when reached, the rest reads\n");
    {
        orbiscore::FisheyeDemux g;
        CHECK(g.open(parts, err));
        std::filesystem::rename(paths[10], paths[10] + ".gone");
        int i = 0;
        while ((i = next(g)) >= 0 && i < 9) {
        }
        CHECK(i == 9);
        orbiscore::Image a, b;
        CHECK(!g.readNextPair(a, b, nullptr, err));
        CHECK(err.find("(part 11)") != std::string::npos && err.find(paths[10]) != std::string::npos);
        err.clear();
        CHECK(!g.seekTo(orbiscore::MsOfUs(startUs(10)), err) && err.find("(part 11)") != std::string::npos);
        err.clear();
        CHECK(g.seekTo(orbiscore::MsOfUs(startUs(12)), err) && next(g) == 12);
        std::filesystem::rename(paths[10] + ".gone", paths[10]);
        CHECK(g.seekTo(orbiscore::MsOfUs(startUs(10)), err) && next(g) == 10); // back
    }

    std::printf("  ... a photo unlike the first: refused when reached\n");
    {
        const std::string odd = dir + "/photo_odd.insp";
        CHECK(testmedia::writeJpeg(odd, 256, 64, [](int, int) { return uint8_t(128); }));
        std::vector<orbiscore::ClipPart> withOdd = parts;
        withOdd[5].lenses = {{odd, 0, left}, {odd, 0, right}};
        orbiscore::FisheyeDemux g;
        CHECK(g.open(withOdd, err));
        CHECK(!g.seekTo(orbiscore::MsOfUs(startUs(5)), err));
        CHECK(err.find("isn't coded like") != std::string::npos && err.find("(part 6)") != std::string::npos &&
              err.find(odd) != std::string::npos);
        err.clear();
        CHECK(g.seekTo(orbiscore::MsOfUs(startUs(6)), err) && next(g) == 6);
        std::filesystem::remove(odd);
    }
    for (const std::string& p : paths)
        std::filesystem::remove(p);
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
    testPairPictureBytes(a, b, dir.string());
    testMissingFrames(dir.string());
    testPhotoSeries(dir.string());
    testFrameState();
    if (!g_failures)
        std::filesystem::remove_all(dir);
    std::printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures;
}
