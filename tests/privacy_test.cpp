// src/privacy/PlateScanData: frame lookup, span coverage, merging scans of
// one source, and the cache file round trip.

#include "PlateScanData.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <numbers>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

// A scan of [fromMs, toMs] at 30 fps; every frame has one region whose first
// corner's x carries the frame time (to tell scans apart after a merge) and
// `tag` in y.
privacy::PlateScan makeScan(int64_t fromMs, int64_t toMs, float tag)
{
    privacy::PlateScan s;
    s.fps = 30.0;
    s.plates = 1;
    s.model = "test.param";
    for (double t = fromMs; t <= toMs + 0.5; t += 1000.0 / 30.0) {
        privacy::ScanFrame f;
        f.tMs = std::llround(t);
        privacy::BlurRegion r;
        r.quad.corner[0][0] = static_cast<float>(f.tMs);
        r.quad.corner[0][1] = tag;
        r.kind = privacy::RegionKind::Tracked;
        f.regions.push_back(r);
        s.frames.push_back(f);
    }
    s.spans.push_back({s.frames.front().tMs, s.frames.back().tMs});
    return s;
}

void testLookup()
{
    const privacy::PlateScan s = makeScan(1000, 2000, 1.0f);
    CHECK(s.frameAt(1000) && s.frameAt(1000)->tMs == 1000);
    CHECK(s.frameAt(1040) && s.frameAt(1040)->tMs == 1033); // nearest
    CHECK(!s.frameAt(900));                                  // before the scan
    CHECK(!s.frameAt(2100));                                 // after it
    std::vector<insv::BlurQuad> q;
    s.quadsAt(1500, q);
    CHECK(q.size() == 1);
    s.quadsAt(5000, q);
    CHECK(q.size() == 1); // nothing added outside

    CHECK(s.covers(1000, 2000));
    CHECK(s.covers(1010, 1990));
    CHECK(s.covers(1000, 2030)); // a clip's out point a frame past the last frame
    CHECK(!s.covers(900, 2000));
    CHECK(!s.covers(1000, 2500));
}

void testMerge()
{
    privacy::PlateScan s = makeScan(1000, 2000, 1.0f);
    s.merge(makeScan(5000, 6000, 2.0f)); // a second clip, elsewhere in the file
    CHECK(s.spans.size() == 2);
    CHECK(s.covers(1000, 2000) && s.covers(5000, 6000));
    CHECK(!s.covers(1000, 6000)); // the gap between isn't scanned
    CHECK(s.plates == 2);

    // A rescan over the middle of the first span: it replaces those frames,
    // the rest of the old span stays, and they join into one span.
    s.merge(makeScan(1400, 1600, 3.0f));
    CHECK(s.spans.size() == 2);
    CHECK(s.covers(1000, 2000));
    CHECK(s.frameAt(1500) && s.frameAt(1500)->regions[0].quad.corner[0][1] == 3.0f);
    CHECK(s.frameAt(1200) && s.frameAt(1200)->regions[0].quad.corner[0][1] == 1.0f);
    CHECK(s.frameAt(1800) && s.frameAt(1800)->regions[0].quad.corner[0][1] == 1.0f);
    for (size_t i = 1; i < s.frames.size(); ++i)
        CHECK(s.frames[i - 1].tMs < s.frames[i].tMs);

    // One that bridges the two spans makes one.
    s.merge(makeScan(1900, 5100, 4.0f));
    CHECK(s.spans.size() == 1);
    CHECK(s.covers(1000, 6000));
}

// The stitch a span was scanned in: kept through a merge, spans of different
// stitches don't join, and staleOver reports a mismatch only where the clip's
// range has one.
void testTiming()
{
    using privacy::StitchTiming;
    CHECK(!privacy::timingStale(StitchTiming::Corrected, true));
    CHECK(privacy::timingStale(StitchTiming::Corrected, false));
    CHECK(privacy::timingStale(StitchTiming::Uncorrected, true));
    CHECK(!privacy::timingStale(StitchTiming::Uncorrected, false));
    CHECK(!privacy::timingStale(StitchTiming::NoGyro, true) && !privacy::timingStale(StitchTiming::NoGyro, false));

    privacy::PlateScan s = makeScan(1000, 2000, 1.0f); // Corrected by default
    privacy::PlateScan off = makeScan(2000, 3000, 2.0f);
    off.spans[0].timing = StitchTiming::Uncorrected;
    s.merge(off); // touches, but scanned the other way: two spans
    CHECK(s.spans.size() == 2 && s.spans[0].timing == StitchTiming::Corrected &&
          s.spans[1].timing == StitchTiming::Uncorrected);
    CHECK(s.covers(1000, 3000));
    CHECK(!s.staleOver(1000, 1900, true) && s.staleOver(1000, 1900, false));
    CHECK(s.staleOver(2100, 2900, true) && !s.staleOver(2100, 2900, false));
    CHECK(s.staleOver(1000, 3000, true) && s.staleOver(1000, 3000, false));
    CHECK(!s.staleOver(5000, 6000, true)); // nothing scanned there
    // A rescan of the middle in the other stitch cuts both, keeping theirs.
    privacy::PlateScan mid = makeScan(1500, 2500, 3.0f);
    mid.spans[0].timing = StitchTiming::NoGyro;
    s.merge(mid);
    CHECK(s.spans.size() == 3 && s.spans[0].timing == StitchTiming::Corrected &&
          s.spans[1].timing == StitchTiming::NoGyro && s.spans[2].timing == StitchTiming::Uncorrected);
    CHECK(!s.staleOver(1600, 2400, true) && !s.staleOver(1600, 2400, false));
}

// A quad of half-size `h` (radians) around the direction at yaw / pitch.
insv::BlurQuad quadAround(double yaw, double pitch, double h = 0.01)
{
    insv::BlurQuad q;
    const double c[4][2] = {{-h, h}, {h, h}, {h, -h}, {-h, -h}}; // TL TR BR BL
    for (int k = 0; k < 4; ++k) {
        const double y = yaw + c[k][0], p = pitch + c[k][1];
        q.corner[k][0] = static_cast<float>(std::cos(p) * std::sin(y));
        q.corner[k][1] = static_cast<float>(std::sin(p));
        q.corner[k][2] = static_cast<float>(-std::cos(p) * std::cos(y));
    }
    return q;
}

void dirAt(double yaw, double pitch, float d[3])
{
    d[0] = static_cast<float>(std::cos(pitch) * std::sin(yaw));
    d[1] = static_cast<float>(std::sin(pitch));
    d[2] = static_cast<float>(-std::cos(pitch) * std::cos(yaw));
}

// Frames [0, n) at 30 fps; track `rigId` sits still at yaw 3, pitch -0.7 (a
// false hit on the rider) over frames [rigFrom, rigTo]; track `carId` moves
// across from yaw -0.5 over frames [carFrom, n).
privacy::PlateScan rigScan(int n, int rigId, int rigFrom, int rigTo, int carId, int carFrom)
{
    privacy::PlateScan s;
    s.fps = 30.0;
    for (int i = 0; i < n; ++i) {
        privacy::ScanFrame f;
        f.tMs = std::llround(i * 1000.0 / 30.0);
        if (i >= rigFrom && i <= rigTo)
            f.regions.push_back({quadAround(3.0 + 0.002 * (i % 3), -0.7), privacy::RegionKind::Tracked, rigId});
        if (i >= carFrom)
            f.regions.push_back({quadAround(-0.5 + 0.01 * i, -0.1), privacy::RegionKind::Detected, carId});
        s.frames.push_back(f);
    }
    s.spans.push_back({s.frames.front().tMs, s.frames.back().tMs});
    return s;
}

void testRemoval()
{
    float d[3];
    const insv::BlurQuad q = quadAround(1.0, 0.2);
    dirAt(1.0, 0.2, d);
    CHECK(privacy::quadContains(q, d));
    dirAt(1.0 + 0.009, 0.2 - 0.009, d);
    CHECK(privacy::quadContains(q, d));
    dirAt(1.02, 0.2, d);
    CHECK(!privacy::quadContains(q, d));
    dirAt(1.0 + std::numbers::pi, -0.2, d); // the antipode
    CHECK(!privacy::quadContains(q, d));

    const privacy::PlateScan s = rigScan(90, 7, 10, 60, 8, 20);
    int32_t track = -1;
    dirAt(3.0, -0.7, d);
    CHECK(s.regionAt(1000, d, &track) && track == 7); // frame 30
    dirAt(0.0, 0.0, d);
    CHECK(!s.regionAt(1000, d, &track));

    // Remove the rider track: its regions go, the car's stay.
    const privacy::BlurRemoval whole = s.describeRemoval(7, INT64_MIN);
    CHECK(whole.wholeTrack && whole.fromMs == s.frames[10].tMs && whole.toMs == s.frames[60].tMs);
    CHECK(whole.path.size() >= 17 && whole.path.back().tMs == s.frames[60].tMs);
    privacy::RemovedSpans rs = s.resolveRemovals({whole});
    CHECK(rs.size() == 1 && rs[0].track == 7);
    std::vector<insv::BlurQuad> out;
    s.quadsAt(s.frames[30].tMs, out, &rs);
    CHECK(out.size() == 1); // the car only
    out.clear();
    s.quadsAt(s.frames[15].tMs, out, &rs);
    CHECK(out.empty());

    // From frame 40 on only.
    const privacy::BlurRemoval part = s.describeRemoval(7, s.frames[40].tMs);
    CHECK(!part.wholeTrack && part.fromMs == s.frames[40].tMs);
    rs = s.resolveRemovals({part});
    out.clear();
    s.quadsAt(s.frames[30].tMs, out, &rs);
    CHECK(out.size() == 2);
    out.clear();
    s.quadsAt(s.frames[50].tMs, out, &rs);
    CHECK(out.size() == 1);

    // A rescan: other track ids, the rider track a little longer. The
    // removal still finds it -- and only it -- and covers its new extent.
    const privacy::PlateScan re = rigScan(90, 2, 5, 70, 3, 20);
    rs = re.resolveRemovals({whole});
    CHECK(rs.size() == 1 && rs[0].track == 2);
    out.clear();
    re.quadsAt(re.frames[6].tMs, out, &rs);
    CHECK(out.empty());
    out.clear();
    re.quadsAt(re.frames[68].tMs, out, &rs);
    CHECK(out.size() == 1);
    // A scan where nothing sits there: nothing removed.
    const privacy::PlateScan none = rigScan(90, 2, 1000, 1000, 3, 20);
    CHECK(none.resolveRemovals({whole}).empty());

    // A merge renumbers the newer scan's tracks past ours.
    privacy::PlateScan m = rigScan(90, 7, 10, 60, 8, 20);
    privacy::PlateScan later = rigScan(30, 0, 0, 29, 1, 40);
    for (privacy::ScanFrame& f : later.frames)
        f.tMs += 10000;
    later.spans = {{later.frames.front().tMs, later.frames.back().tMs}};
    m.merge(later);
    const privacy::ScanFrame* lf = m.frameAt(10000 + 500);
    CHECK(lf && lf->regions.size() == 1 && lf->regions[0].track == 9);
}

void testAdded()
{
    // A plate blurred by hand over frames 10..20 at 30 fps, at yaw 0.5.
    privacy::BlurAddition a;
    for (int i = 10; i <= 20; ++i) {
        privacy::BlurAddition::Frame f;
        f.tMs = std::llround(i * 1000.0 / 30.0);
        const insv::BlurQuad q = quadAround(0.5 + 0.001 * i, 0.0);
        std::copy(&q.corner[0][0], &q.corner[0][0] + 12, &f.corner[0][0]);
        a.frames.push_back(f);
    }
    CHECK(privacy::addedFrameAt(a, 500, 30.0) && privacy::addedFrameAt(a, 500, 30.0)->tMs == 500);
    CHECK(privacy::addedFrameAt(a, 510, 30.0) && privacy::addedFrameAt(a, 510, 30.0)->tMs == 500); // nearest
    CHECK(!privacy::addedFrameAt(a, 300, 30.0)); // before it
    CHECK(!privacy::addedFrameAt(a, 700, 30.0)); // after it
    std::vector<insv::BlurQuad> out;
    privacy::addedQuadsAt({a, a}, 400, 30.0, out);
    CHECK(out.size() == 2);
    out.clear();
    privacy::addedQuadsAt({a}, 100, 30.0, out);
    CHECK(out.empty());
    float d[3];
    dirAt(0.5 + 0.015, 0.0, d);
    CHECK(privacy::addedAt({a}, 500, 30.0, d) == 0);
    CHECK(privacy::addedAt({a, a}, 500, 30.0, d) == 1); // the last one is on top
    dirAt(0.0, 0.0, d);
    CHECK(privacy::addedAt({a}, 500, 30.0, d) == -1);
}

void testFile()
{
    privacy::PlateScan s = makeScan(1000, 2000, 1.0f);
    privacy::PlateScan off = makeScan(5000, 5500, 2.0f);
    off.spans[0].timing = privacy::StitchTiming::Uncorrected;
    s.merge(off);
    const std::string path = "privacy_test.scan";
    std::string err;
    CHECK(privacy::savePlateScan(s, path, err));
    privacy::PlateScan r;
    CHECK(privacy::loadPlateScan(path, r, err));
    CHECK(r.spans.size() == 2 && r.spans[1].fromMs == s.spans[1].fromMs && r.spans[1].toMs == s.spans[1].toMs);
    CHECK(r.frames.size() == s.frames.size());
    CHECK(r.plates == 2 && r.model == "test.param" && r.fps == 30.0);
    CHECK(r.frames[5].regions.size() == 1 && r.frames[5].regions[0].kind == privacy::RegionKind::Tracked);
    CHECK(r.frames.back().regions[0].track == s.frames.back().regions[0].track &&
          r.frames.back().regions[0].track == 1); // the merged scan's, renumbered
    CHECK(r.frames[5].regions[0].quad.corner[0][0] == s.frames[5].regions[0].quad.corner[0][0]);
    CHECK(r.spans.size() == 2 && r.spans[0].timing == privacy::StitchTiming::Corrected &&
          r.spans[1].timing == privacy::StitchTiming::Uncorrected);
    std::remove(path.c_str());

    // A v2 cache (no timing per span; scanned with the correction) still
    // reads: one span, no frames.
    {
        FILE* v2 = std::fopen(path.c_str(), "wb");
        const char magic[8] = {'O', 'R', 'P', 'L', 'A', 'T', 'E', 'S'};
        const uint32_t version = 2, nSpans = 1, modelLen = 0;
        const double fps = 30.0;
        const int32_t plates = 0;
        const uint64_t nFrames = 0;
        const int64_t span[2] = {100, 900};
        std::fwrite(magic, sizeof magic, 1, v2);
        std::fwrite(&version, sizeof version, 1, v2);
        std::fwrite(&nSpans, sizeof nSpans, 1, v2);
        std::fwrite(&fps, sizeof fps, 1, v2);
        std::fwrite(&plates, sizeof plates, 1, v2);
        std::fwrite(&modelLen, sizeof modelLen, 1, v2);
        std::fwrite(&nFrames, sizeof nFrames, 1, v2);
        std::fwrite(span, sizeof span, 1, v2);
        std::fclose(v2);
        privacy::PlateScan old;
        CHECK(privacy::loadPlateScan(path, old, err));
        CHECK(old.spans.size() == 1 && old.spans[0].fromMs == 100 && old.spans[0].toMs == 900 &&
              old.spans[0].timing == privacy::StitchTiming::Corrected);
        std::remove(path.c_str());
    }

    CHECK(!privacy::loadPlateScan("does-not-exist.scan", r, err));
    // A file that isn't a scan.
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fputs("not a scan", f);
    std::fclose(f);
    CHECK(!privacy::loadPlateScan(path, r, err));
    std::remove(path.c_str());
}

} // namespace

int main()
{
    testLookup();
    testMerge();
    testTiming();
    testRemoval();
    testAdded();
    testFile();
    if (g_failures == 0)
        std::printf("privacy_test: all checks passed\n");
    else
        std::printf("privacy_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
