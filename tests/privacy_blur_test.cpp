// src/ui/PrivacyBlur: a clip's privacy blur resolved against its source's
// plate scan -- the Privacy panel's status line, what blocks or warns about
// an export, the blur handed to the export, and what a right click on the
// picture finds and edits; and the scans going along with Save as. A
// synthetic scan in a temporary cache, no GPU.

#include "PrivacyBlur.h"

#include "PlaybackController.h"
#include "ProjectFiles.h"
#include "../core/ClipItem.h"
#include "../core/Project.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <climits>
#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

// A small quad around the direction (x, 0, 0), x = +1 or -1.
insv::BlurQuad quadAround(float x)
{
    const float a = 0.05f;
    const float corners[4][3] = {{x, -a, a}, {x, a, a}, {x, a, -a}, {x, -a, -a}};
    insv::BlurQuad q;
    for (int k = 0; k < 4; ++k) {
        const float n = std::sqrt(corners[k][0] * corners[k][0] + corners[k][1] * corners[k][1] +
                                  corners[k][2] * corners[k][2]);
        for (int i = 0; i < 3; ++i)
            q.corner[k][i] = corners[k][i] / n;
    }
    return q;
}

// A scan of [fromMs, toMs] at 30 fps with one plate (track 1) at +x.
privacy::PlateScan makeScan(int64_t fromMs, int64_t toMs)
{
    privacy::PlateScan s;
    s.fps = 30.0;
    s.plates = 1;
    for (double t = fromMs; t <= toMs + 0.5; t += 1000.0 / 30.0) {
        privacy::ScanFrame f;
        f.tMs = std::llround(t);
        privacy::BlurRegion r;
        r.quad = quadAround(1.0f);
        r.kind = privacy::RegionKind::Detected;
        r.track = 1;
        f.regions.push_back(r);
        s.frames.push_back(f);
    }
    s.spans.push_back({s.frames.front().tMs, s.frames.back().tMs});
    return s;
}

size_t quadsAt(const stitch::BlurQuadsFn &fn, int64_t ms)
{
    std::vector<insv::BlurQuad> out;
    if (fn)
        fn(ms, out);
    return out.size();
}

const float kAtPlate[3] = {1.0f, 0.0f, 0.0f};
const float kAway[3] = {-1.0f, 0.0f, 0.0f};

void testScanAndRemovals(const QString &dir)
{
    const QString source = dir + "/a.insv";
    {
        QFile f(source);
        CHECK(f.open(QIODevice::WriteOnly) && f.write("not a real clip") > 0);
    }
    PrivacyBlur pb;
    pb.scans().setDirectory(dir);

    ClipItem c;
    c.sourcePath = source;
    c.trimInMs = 1000;
    c.trimOutMs = 2000;
    c.fps = 30.0;
    c.blurPlates = true;

    // Nothing scanned: the blur is on but the export is refused.
    CHECK(pb.status(c) == "Not scanned yet. Exports with the blur on are refused until it is.");
    CHECK(pb.blocker(c, c.trimInMs, c.trimOutMs).contains("hasn't been scanned"));
    CHECK(!pb.blurFor(c));
    CHECK(pb.hitAt(c, 1500, kAtPlate, true).added < 0 && !pb.hitAt(c, 1500, kAtPlate, true).region);

    // Part of the clip scanned.
    CHECK(pb.scans().store(source, makeScan(1000, 1500)));
    CHECK(pb.status(c).startsWith("Partly scanned."));
    CHECK(pb.blocker(c, c.trimInMs, c.trimOutMs).contains("only part"));
    CHECK(pb.blocker(c, 1000, 1400).isEmpty()); // a screenshot in the scanned part

    // All of it.
    CHECK(pb.scans().store(source, makeScan(1500, 2000)));
    CHECK(pb.status(c) == "Scanned: plates on 30 of 30 frames.");
    CHECK(pb.blocker(c, c.trimInMs, c.trimOutMs).isEmpty());
    CHECK(quadsAt(pb.blurFor(c), 1500) == 1);
    c.blurPlates = false;
    CHECK(!pb.blurFor(c)); // the scan is only applied with the blur on
    CHECK(pb.blocker(c, c.trimInMs, c.trimOutMs).isEmpty());
    CHECK(!pb.hitAt(c, 1500, kAtPlate, false).region); // nothing shown to point at
    c.blurPlates = true;

    // The right click finds the plate's track, not the empty sky.
    PrivacyBlur::Hit hit = pb.hitAt(c, 1500, kAtPlate, false);
    CHECK(hit.added < 0 && hit.region && !hit.removed); // its track: merge() renumbers
    CHECK(!pb.hitAt(c, 1500, kAway, false).region);

    // Remove the track: gone from the blur, found again only with the
    // outlines on, and restored.
    const auto removed = pb.withRemoved(c, hit.track, INT64_MIN);
    CHECK(removed && removed->size() == 1);
    CHECK(!pb.withRemoved(c, 7, INT64_MIN)); // no such track
    if (removed)
        c.blurRemovals = *removed;
    CHECK(quadsAt(pb.blurFor(c), 1500) == 0);
    CHECK(!pb.hitAt(c, 1500, kAtPlate, false).region);
    hit = pb.hitAt(c, 1500, kAtPlate, true);
    CHECK(hit.region && hit.removed);
    const auto restored = pb.withRestored(c, hit.track, 1500);
    CHECK(restored && restored->empty());

    // Stale: the scan was made in the corrected stitch (stabilization on),
    // and the clip's stabilization is off.
    c.blurRemovals.clear();
    CHECK(pb.staleness(c, c.trimInMs, c.trimOutMs, false)
              .startsWith("The plate scan was made with the stabilization on"));
    CHECK(pb.staleness(c, c.trimInMs, c.trimOutMs, true).startsWith("“a.insv”: "));
    c.stabilization.mode = insv::StabMode::HorizonLock;
    CHECK(pb.staleness(c, c.trimInMs, c.trimOutMs, false).isEmpty());

    // No scan for another source.
    ClipItem other = c;
    other.sourcePath = dir + "/missing.insv";
    CHECK(!pb.withRemoved(other, 1, INT64_MIN) && !pb.withRestored(other, 1, 1500));
}

void testHandBlurs(const QString &dir)
{
    PrivacyBlur pb; // no scan for this source
    pb.scans().setDirectory(dir);
    ClipItem c;
    c.sourcePath = "/nonexistent/b.insv";
    c.trimInMs = 0;
    c.trimOutMs = 1000;
    c.fps = 30.0;
    privacy::BlurAddition a;
    for (int64_t t = 0; t <= 300; t += 33) {
        privacy::BlurAddition::Frame f;
        f.tMs = t;
        const insv::BlurQuad q = quadAround(1.0f);
        std::copy(&q.corner[0][0], &q.corner[0][0] + 12, &f.corner[0][0]);
        a.frames.push_back(f);
    }
    c.blurAdditions = {a};

    // Applied with the scan's blur off too.
    CHECK(quadsAt(pb.blurFor(c), 99) == 1);
    CHECK(quadsAt(pb.blurFor(c), 600) == 0);
    CHECK(pb.status(c) == "Not scanned yet. 1 plate blurred by hand.");
    c.blurAdditions.push_back(a);
    CHECK(pb.status(c) == "Not scanned yet. 2 plates blurred by hand.");
    c.blurAdditions.pop_back();

    const PrivacyBlur::Hit hit = pb.hitAt(c, 99, kAtPlate, false);
    CHECK(hit.added == 0 && !hit.region);
    CHECK(pb.hitAt(c, 99, kAway, false).added < 0);

    // "Remove this blur from here on" keeps the frames before; "Remove this
    // blur" drops it.
    const auto cut = PrivacyBlur::cutAddition(c, 0, 99 - 1); // the playhead on the frame at 99
    CHECK(cut.size() == 1 && cut[0].frames.size() == 3 && cut[0].frames.back().tMs == 66);
    CHECK(PrivacyBlur::cutAddition(c, 0, INT64_MIN).empty());
    CHECK(PrivacyBlur::cutAddition(c, 5, 0) == c.blurAdditions); // out of range: unchanged

    // Followed in the corrected stitch; the clip's stabilization is off.
    CHECK(pb.staleness(c, 0, 1000, false)
              .startsWith("1 plate blurred by hand was followed with the stabilization on"));
    CHECK(pb.staleness(c, 500, 1000, false).isEmpty()); // not in that range
}

// Playback's blur is per clip, as the export's is: a duplicate of a clip
// (same file, same trim) with the blur off isn't blurred along with it.
void testPlaybackPerClip(const QString &dir)
{
    const QString source = dir + "/d.insv";
    {
        QFile f(source);
        CHECK(f.open(QIODevice::WriteOnly) && f.write("not a real clip, still") > 0);
    }
    PrivacyBlur pb;
    pb.scans().setDirectory(dir);
    CHECK(pb.scans().store(source, makeScan(0, 1000)));

    Timeline t;
    const int first = t.appendClip(source, 1000, 30.0);
    const int copy = t.duplicateClip(first);
    CHECK(first && copy && first != copy);
    t.setClipBlurPlates(first, true);

    PlaybackController controller;
    pb.pushTo(controller, t, false);
    CHECK(quadsAt(controller.blurQuads(first), 500) == 1);
    CHECK(!controller.blurQuads(copy));

    // And the other way round, once the first is off again.
    t.setClipBlurPlates(first, false);
    t.setClipBlurPlates(copy, true);
    pb.pushTo(controller, t, false);
    CHECK(!controller.blurQuads(first));
    CHECK(quadsAt(controller.blurQuads(copy), 500) == 1);
}

// Save as into a new project folder takes the clips' plate scans along
// (they take minutes to redo), leaving one the new folder has alone.
void testScansFollowSaveAs(const QString &dir)
{
    const QString source = dir + "/c.insv";
    {
        QFile f(source);
        CHECK(f.open(QIODevice::WriteOnly) && f.write("not a real clip either") > 0);
    }
    const QString oldFile = projectfolder::create(dir, "Old");
    const QString newFile = projectfolder::create(dir, "New");
    CHECK(!oldFile.isEmpty() && !newFile.isEmpty());
    PlateScanCache old;
    old.setDirectory(projectfolder::cacheDir(oldFile));
    CHECK(old.store(source, makeScan(0, 1000)));

    Project p;
    p.timeline()->appendClip(source, 10000, 30.0);
    ProjectFiles::copyPlateScans(p, oldFile, newFile);
    PlateScanCache copied;
    copied.setDirectory(projectfolder::cacheDir(newFile));
    CHECK(copied.get(source) && copied.get(source)->covers(0, 1000));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    testScanAndRemovals(tmp.path());
    testHandBlurs(tmp.path());
    testScansFollowSaveAs(tmp.path());
    testPlaybackPerClip(tmp.path());
    if (g_failures == 0)
        std::printf("privacy_blur_test: all checks passed\n");
    else
        std::printf("privacy_blur_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
