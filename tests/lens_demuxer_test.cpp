// insv::InsvDemux / LensDemuxer with a file per lens (the ONE X2's layout):
// two single-stream videos written here, each frame a flat grey that encodes
// its index, decoded as one lens pair -- in step, after seeks, to the end,
// and in the order the lenses are given. Exit code is the failure count.

#include "InsvDemux.h"
#include "support/TestMedia.h"

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
int frameOf(const insv::Image& img, int (*grey)(int))
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
std::pair<int, int> nextPair(insv::InsvDemux& d, int (*grey0)(int), int (*grey1)(int))
{
    insv::Image a, b;
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
    insv::InsvDemux d;
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
    insv::InsvDemux d;
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
    insv::InsvDemux d;
    std::string err;
    CHECK(d.open({{b, 0}, {a, 0}}, err));
    CHECK(d.seekTo(400, err));
    CHECK(nextPair(d, greyB, greyA) == std::make_pair(10, 10));
}

void testErrors(const std::string& a, const std::string& dir)
{
    std::printf("  two files: errors name the other file\n");
    insv::InsvDemux d;
    std::string err;
    const std::string gone = dir + "/gone.mp4";
    CHECK(!d.open({{a, 0}, {gone, 0}}, err));
    CHECK(err.find(gone) != std::string::npos);
    err.clear();
    CHECK(!d.open({{a, 0}, {a, 1}}, err)); // one video stream only
    CHECK(err == "no video stream 1");
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
    if (!g_failures)
        std::filesystem::remove_all(dir);
    std::printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures;
}
