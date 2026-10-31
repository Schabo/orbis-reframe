// stitch::SeamAnalysis, the depth-aware seam's per-clip store: states put
// and read back by frame time, the stretches it covers, the cache file read
// back by a new session (and a record torn by a stop mid-write left out),
// and a new analysis when anything the states depend on changes -- the
// seam correction, the lens colour, the lens guard, the file. No GPU: the
// analysis pass itself (AnalyseClipSeam) is checked by tools/seam_regress
// on real clips. Exit code is the failure count.

#include "ClipSeamAnalysis.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// A state that differs per frame, already at the store's half precision.
insv::SeamState stateFor(int64_t pts)
{
    insv::SeamState s;
    for (int b = 0; b < insv::kSeamDepthBins; ++b) {
        s.q[b] = 0.01f * static_cast<float>((b * 7 + pts) % 300);
        s.blend.cut[b] = 0.001f * static_cast<float>(b % 9) - 0.004f;
        s.blend.fade[b] = 0.013f + 0.0001f * static_cast<float>(pts % 50);
        for (int k = 0; k < 3; ++k) {
            s.blend.local[b][k] = 0.002f * static_cast<float>(k - 1) * (b % 5);
            s.blend.veil[b][k] = b > 100 ? 0.01f * k : 0.0f;
        }
    }
    s.blend.colour[0] = 0.031f;
    s.blend.colour[2] = -0.017f;
    s.depthConf = 0.62f;
    insv::QuantiseSeamState(s);
    return s;
}

bool same(const insv::SeamState& a, const insv::SeamState& b)
{
    return std::memcmp(a.q, b.q, sizeof a.q) == 0 &&
           std::memcmp(&a.blend, &b.blend, sizeof a.blend) == 0 && a.depthConf == b.depthConf;
}

void testStore(const std::filesystem::path& clip, const std::filesystem::path& dir)
{
    stitch::SeamAnalysisKey key;
    key.path = clip.string();
    key.seam.lens1RotDeg[1] = 0.37;
    key.colour.logRatio[2] = -0.04;

    // In memory.
    stitch::SetSeamAnalysisDir("");
    auto mem = stitch::SeamAnalysisFor(key);
    CHECK(stitch::SeamAnalysisFor(key) == mem); // the one already open
    insv::SeamState s;
    CHECK(!mem->stateAt(0, s));
    CHECK(!mem->covers(0, 100) && mem->missing(0, 100).size() == 1);
    for (int64_t t = 0; t <= 200; t += 33)
        mem->put(t, stateFor(t));
    CHECK(mem->stateAt(66, s) && same(s, stateFor(66)));
    CHECK(mem->stateAt(67, s) && same(s, stateFor(66))); // a millisecond off: the same frame
    CHECK(!mem->stateAt(50, s));
    mem->markDone(0, 99);
    CHECK(mem->covers(0, 99) && mem->covers(10, 50) && !mem->covers(0, 200));
    {
        const auto gaps = mem->missing(0, 200);
        CHECK(gaps.size() == 1 && gaps[0].first == 99 && gaps[0].second == 200);
    }
    mem->markDone(99, 198);
    CHECK(mem->covers(0, 198) && mem->missing(0, 198).empty());
    CHECK(mem->coverage(0, 396) > 0.49 && mem->coverage(0, 396) < 0.51);

    // In a cache folder, read back by the next session.
    stitch::SetSeamAnalysisDir(dir.string());
    auto disk = stitch::SeamAnalysisFor(key);
    CHECK(disk != mem && !disk->stateAt(66, s)); // memory isn't the folder
    CHECK(!std::filesystem::exists(dir) || std::filesystem::is_empty(dir)); // only looked up: no file
    for (int64_t t = 0; t <= 200; t += 33)
        disk->put(t, stateFor(t));
    disk->markDone(0, 198);
    disk->flush();
    stitch::SetSeamAnalysisDir(""); // a new session ...
    stitch::SetSeamAnalysisDir(dir.string());
    auto again = stitch::SeamAnalysisFor(key);
    CHECK(again != disk);
    CHECK(again->stateAt(132, s) && same(s, stateFor(132)));
    CHECK(again->covers(0, 198));

    // A stop mid-write leaves a torn record at the end: the whole ones are
    // kept, and the next write goes over it.
    std::filesystem::path file;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        file = e.path();
    {
        std::ofstream torn(file, std::ios::binary | std::ios::app);
        const char partial[] = {1, 7, 0, 0, 0};
        torn.write(partial, sizeof partial);
    }
    stitch::SetSeamAnalysisDir("");
    stitch::SetSeamAnalysisDir(dir.string());
    auto torn = stitch::SeamAnalysisFor(key);
    CHECK(torn->stateAt(198, s) && same(s, stateFor(198)));
    torn->put(231, stateFor(231));
    torn->flush();
    CHECK(torn->stateAt(231, s) && same(s, stateFor(231)));
    stitch::SetSeamAnalysisDir("");
    stitch::SetSeamAnalysisDir(dir.string());
    auto after = stitch::SeamAnalysisFor(key);
    CHECK(after->stateAt(231, s) && same(s, stateFor(231)) && after->stateAt(0, s));

    // Anything the states depend on: another analysis.
    auto other = [&](stitch::SeamAnalysisKey k) {
        auto a = stitch::SeamAnalysisFor(k);
        return a != after && !a->stateAt(66, s);
    };
    stitch::SeamAnalysisKey k = key;
    k.seam.lens1RotDeg[1] = 0.38;
    CHECK(other(k));
    k = key;
    k.colour.logRatio[2] = -0.05;
    CHECK(other(k));
    k = key;
    k.guard = stitch::PreviewStitchConfig::LensGuard::None;
    CHECK(other(k));
    CHECK(stitch::SeamAnalysisFor(key) == after);
    // The file itself, changed in place.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::ofstream f(clip, std::ios::binary | std::ios::app);
        f << "x";
    }
    CHECK(other(key));
}

} // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "orbis_seam_analysis_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto clip = root / "clip.insv";
    {
        std::ofstream f(clip, std::ios::binary);
        f << "not a real clip: only its size and time are read";
    }
    testStore(clip, root / "cache" / "seam");
    stitch::SetSeamAnalysisDir("");
    std::filesystem::remove_all(root);
    if (g_failures == 0)
        std::printf("seam_analysis_test: all checks passed\n");
    return g_failures;
}
