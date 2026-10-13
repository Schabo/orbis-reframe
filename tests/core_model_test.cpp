// Standalone checks for the src/core edit model (ClipItem + Timeline). No Qt
// event loop needed: everything here is synchronous, and same-thread signal
// connections fire directly inside emit. Exit code is the failure count.
//
// Covers the trim clamp / min-gap and reframe-snap rules, the Timeline time
// mapping, the multi-clip structure ops (insert / append / remove / move),
// playbackPlan(), and that the aggregate signals fire.

#include "ClipItem.h"
#include "MediaPool.h"
#include "Project.h"
#include "ProjectFolder.h"
#include "ProjectLibrary.h"
#include "Timeline.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

void testClipItemTrim()
{
    ClipItem c;
    c.sourceDurationMs = 10000;
    c.fps = 30.0;
    c.resetEditsToFullSource();
    CHECK(c.trimInMs == 0 && c.trimOutMs == 10000);
    CHECK(c.trimmedDurationMs() == 10000);

    CHECK(c.setTrimIn(3000));
    CHECK(c.trimInMs == 3000 && c.trimOutMs == 10000);
    CHECK(!c.setTrimIn(3000)); // no-op returns false

    // in can't cross out: it's pinned kMinTrimMs below it
    CHECK(c.setTrimIn(999999));
    CHECK(c.trimInMs == 10000 - ClipItem::kMinTrimMs && c.trimOutMs == 10000);

    c.clearTrim();
    CHECK(c.trimInMs == 0 && c.trimOutMs == 10000);

    // out clamps: negative -> 0 -> pulled up to in + gap
    CHECK(c.setTrimOut(-50));
    CHECK(c.trimInMs == 0 && c.trimOutMs == ClipItem::kMinTrimMs);

    // a clip shorter than the min gap always spans whole
    ClipItem tiny;
    tiny.sourceDurationMs = 40;
    tiny.resetEditsToFullSource();
    CHECK(!tiny.setTrimIn(10));
    CHECK(tiny.trimInMs == 0 && tiny.trimOutMs == 40);
}

void testClipItemReframe()
{
    ClipItem c;
    c.sourceDurationMs = 10000;
    c.fps = 25.0; // frameDurationMs == 40
    c.resetEditsToFullSource();
    CHECK(c.frameDurationMs() == 40);

    CHECK(c.upsertReframeKey(1000, {0.5f, 0.0f, 0.0f, 1.3f})); // {yaw,pitch,roll,fovY}
    CHECK(c.reframe.count() == 1);
    CHECK(c.upsertReframeKey(1020, {0.6f, 0.0f, 0.2f, 1.3f})); // within a frame -> overwrite
    CHECK(c.reframe.count() == 1);
    CHECK(c.reframe.keys()[0].pose.roll > 0.19f && c.reframe.keys()[0].pose.roll < 0.21f);
    CHECK(!c.removeReframeKeyNear(5000)); // nothing near
    CHECK(c.removeReframeKeyNear(1010));  // within the delete snap
    CHECK(c.reframe.isEmpty());
    CHECK(!c.clearReframe()); // already empty

    // roll interpolates shortest-arc like yaw, across the +/-pi seam
    c.upsertReframeKey(0, {0.0f, 0.0f, 3.0f, 1.3f});
    c.upsertReframeKey(1000, {0.0f, 0.0f, -3.0f, 1.3f});
    const float midRoll = c.reframe.sample(500).roll;
    CHECK(c.reframe.sample(-100).roll == 3.0f && c.reframe.sample(2000).roll == -3.0f); // held
    CHECK(midRoll > 3.0f || midRoll < -3.0f); // went the short way (through +/-pi), not through 0
    c.clearReframe();

    // replaceSpan (Track subject): keys inside [from, to] go, the new ones
    // land sorted (clamped into the span), keys outside stay
    c.upsertReframeKey(500, {0.1f, 0.0f, 0.0f, 1.3f});
    c.upsertReframeKey(1500, {0.2f, 0.0f, 0.0f, 1.3f});
    c.upsertReframeKey(2500, {0.3f, 0.0f, 0.0f, 1.3f});
    c.upsertReframeKey(4000, {0.4f, 0.0f, 0.0f, 1.3f});
    c.reframe.replaceSpan(1000, 3000,
                          {{2000, {1.0f, 0.1f, 0.0f, 1.0f}}, {1000, {0.9f, 0.1f, 0.0f, 1.0f}},
                           {3500, {1.1f, 0.1f, 0.0f, 1.0f}}});
    CHECK(c.reframe.count() == 5);
    CHECK(c.reframe.keyTimes() == QList<qint64>({500, 1000, 2000, 3000, 4000}));
    CHECK(c.reframe.keys()[1].pose.yaw > 0.89f && c.reframe.keys()[1].pose.yaw < 0.91f);
    CHECK(c.reframe.keys()[3].pose.yaw > 1.09f); // the 3500 key, clamped to 3000
    CHECK(c.reframe.keys()[4].pose.yaw > 0.39f && c.reframe.keys()[4].pose.yaw < 0.41f);
    c.clearReframe();

    // resetEditsToFullSource wipes an authored path
    c.upsertReframeKey(2000, {});
    c.resetEditsToFullSource();
    CHECK(c.reframe.isEmpty());
}

// InsetKeyTrack's key handling directly (it shares keytrack:: with
// ReframeTrack): snap-overwrite keeps the key's time, the look is clamped,
// keys stay sorted, the nearest key within the snap goes, and sample() holds
// before the first / after the last key and is linear between.
void testInsetKeyTrack()
{
    auto look = [](float cx, float opacity) {
        inset::Look l;
        l.centerX = cx;
        l.opacity = opacity;
        return l;
    };
    InsetKeyTrack k;
    CHECK(k.sample(500) == inset::Look{}); // empty: the default look
    k.upsert(3000, look(0.8f, 1.0f), 40);
    k.upsert(1000, look(0.2f, 2.0f), 40); // opacity clamped to 1
    CHECK(k.count() == 2 && k.keys()[0].timeMs == 1000 && k.keys()[1].timeMs == 3000);
    CHECK(k.keys()[0].look.opacity == 1.0f);
    k.upsert(1030, look(0.4f, 0.5f), 40); // within the snap -> overwrite, time kept
    CHECK(k.count() == 2 && k.keys()[0].timeMs == 1000 && k.keys()[0].look.centerX == 0.4f);

    CHECK(k.sample(0).centerX == 0.4f);    // held before the first key
    CHECK(k.sample(9000).centerX == 0.8f); // and after the last
    const inset::Look mid = k.sample(2000);
    CHECK(std::abs(mid.centerX - 0.6f) < 1e-5f && std::abs(mid.opacity - 0.75f) < 1e-5f);

    k.upsert(1100, look(0.5f, 1.0f), 40); // 70 ms from 1000: a new key
    CHECK(k.count() == 3 && k.keys()[1].timeMs == 1100);
    CHECK(!k.removeNear(2000, 40));
    CHECK(k.removeNear(1080, 100)); // both within 100: the nearer (1100) goes
    CHECK(k.count() == 2 && k.keys()[0].timeMs == 1000 && k.keys()[1].timeMs == 3000);
}

// Which kind of mic an .insv audio stream is (docs/INSV_FORMAT.md): with
// two streams the first is the external mic, else the only one is the
// camera's.
void testStreamRoles()
{
    using R = Track::Role;
    CHECK(Track::roleOfStream(0, 1) == R::CameraMic);
    CHECK(Track::roleOfStream(0, 2) == R::ExternalMic);
    CHECK(Track::roleOfStream(1, 2) == R::CameraMic);
    CHECK(Track::roleOfStream(2, 3) == R::Free); // no known kind
    CHECK(Track::roleOfStream(1, 1) == R::Free);
}

void testClipItemColor()
{
    ClipItem c;
    CHECK(c.color == ColorGrade{}); // defaults, all neutral

    ColorGrade g;
    g.exposureEv = 1.5f;
    g.contrast = -20.0f;
    g.saturation = 40.0f;
    g.temperature = -10.0f;
    CHECK(c.setColorGrade(g));
    CHECK(c.color == g);
    CHECK(!c.setColorGrade(g)); // no-op returns false

    // clamped to each field's own range
    ColorGrade extreme;
    extreme.exposureEv = 999.0f;
    extreme.contrast = 999.0f;
    extreme.saturation = -999.0f;
    extreme.temperature = -999.0f;
    CHECK(c.setColorGrade(extreme));
    CHECK(c.color.exposureEv == ColorGrade::kMaxExposureEv);
    CHECK(c.color.contrast == ColorGrade::kMaxPercent);
    CHECK(c.color.saturation == ColorGrade::kMinPercent);
    CHECK(c.color.temperature == ColorGrade::kMinPercent);
}

void testTimelineMapping()
{
    Timeline tl;
    CHECK(tl.isEmpty());
    CHECK(tl.timelineToClip(0).clipIndex == -1);
    CHECK(tl.totalDurationMs() == 0);

    const int a = tl.appendClip("a.insv", 10000, 30.0);
    const int b = tl.appendClip("b.insv", 6000, 30.0);
    CHECK(tl.count() == 2);
    CHECK(a == 1 && b == 2); // ids are monotonic from 1
    CHECK(tl.totalDurationMs() == 16000);

    // trim a to [2000, 8000] -> it contributes 6000
    tl.setClipTrimIn(a, 2000);
    tl.setClipTrimOut(a, 8000);
    CHECK(tl.clipById(a)->trimmedDurationMs() == 6000);
    CHECK(tl.totalDurationMs() == 12000);

    auto p = tl.timelineToClip(3000);
    CHECK(p.clipIndex == 0 && p.clipLocalMs == 3000);
    p = tl.timelineToClip(6000); // the cut boundary belongs to the later clip
    CHECK(p.clipIndex == 1 && p.clipLocalMs == 0);
    p = tl.timelineToClip(9000);
    CHECK(p.clipIndex == 1 && p.clipLocalMs == 3000);
    p = tl.timelineToClip(999999); // past the end -> last clip, full span
    CHECK(p.clipIndex == 1 && p.clipLocalMs == 6000);
    p = tl.timelineToClip(-100); // before the start
    CHECK(p.clipIndex == 0 && p.clipLocalMs == 0);

    CHECK(tl.clipToTimeline(0, 3000) == 3000);
    CHECK(tl.clipToTimeline(1, 0) == 6000);
    CHECK(tl.clipToTimeline(1, 3000) == 9000);
    CHECK(tl.clipToTimeline(1, 999999) == 12000); // clamped to b's span
    CHECK(tl.clipToTimeline(5, 0) == 0);          // out-of-range index
}

void testClipSpeed()
{
    Timeline tl;
    const int a = tl.appendClip("a.insv", 10000, 120.0);
    const int b = tl.appendClip("b.insv", 6000, 30.0);
    tl.setClipSpeed(a, 0.25); // 4x slow motion: a fills 40 s of timeline
    const ClipItem &ca = *tl.clipById(a);
    CHECK(ca.speed == 0.25);
    CHECK(ca.trimmedDurationMs() == 10000);
    CHECK(ca.timelineDurationMs() == 40000);
    CHECK(tl.totalDurationMs() == 46000);

    auto p = tl.timelineToClip(20000);
    CHECK(p.clipIndex == 0 && p.clipLocalMs == 20000);
    CHECK(ca.sourceAtLocalMs(p.clipLocalMs) == 5000); // timeline-local -> source
    CHECK(ca.localAtSourceMs(5000) == 20000);
    p = tl.timelineToClip(40000); // the cut belongs to b
    CHECK(p.clipIndex == 1 && p.clipLocalMs == 0);
    CHECK(tl.clipToTimeline(1, 1000) == 41000);

    // Trims stay in source time; the span follows them.
    tl.setClipTrimIn(a, 2000);
    CHECK(tl.clipById(a)->timelineDurationMs() == 32000);
    CHECK(tl.clipById(a)->sourceAtLocalMs(4000) == 3000);

    // Speed-up, clamping, junk.
    tl.setClipSpeed(b, 2.0);
    CHECK(tl.clipById(b)->timelineDurationMs() == 3000);
    tl.setClipSpeed(b, 100.0);
    CHECK(tl.clipById(b)->speed == ClipItem::kMaxSpeed);
    tl.setClipSpeed(b, 0.0);
    CHECK(tl.clipById(b)->speed == ClipItem::kMinSpeed);
    tl.setClipSpeed(b, std::numeric_limits<double>::quiet_NaN());
    CHECK(tl.clipById(b)->speed == ClipItem::kMinSpeed); // ignored

    // A split keeps the speed on both halves.
    const int right = tl.splitClip(a, 6000);
    CHECK(right != 0 && tl.clipById(right)->speed == 0.25 && tl.clipById(a)->speed == 0.25);
    CHECK(tl.clipById(a)->timelineDurationMs() == 16000);     // source [2000, 6000]
    CHECK(tl.clipById(right)->timelineDurationMs() == 16000); // source [6000, 10000]
}

void testTimelineStructure()
{
    Timeline tl;
    int inserted = 0, removed = 0, reordered = 0, trimTouched = 0, reframeTouched = 0;
    qint64 lastDuration = -1;
    QObject::connect(&tl, &Timeline::clipInserted, &tl, [&](int) { ++inserted; });
    QObject::connect(&tl, &Timeline::clipRemoved, &tl, [&](int) { ++removed; });
    QObject::connect(&tl, &Timeline::clipsReordered, &tl, [&] { ++reordered; });
    QObject::connect(&tl, &Timeline::clipTrimChanged, &tl,
                     [&](int, qint64, qint64) { ++trimTouched; });
    QObject::connect(&tl, &Timeline::clipReframeChanged, &tl, [&](int) { ++reframeTouched; });
    QObject::connect(&tl, &Timeline::durationChanged, &tl, [&](qint64 d) { lastDuration = d; });

    const int a = tl.appendClip("a.insv", 4000, 30.0);
    const int c = tl.appendClip("c.insv", 4000, 30.0);
    // insert b between them
    const int b = tl.insertClip(1, "b.insv", 4000, 30.0);
    CHECK(inserted == 3);
    CHECK(tl.count() == 3);
    CHECK(tl.indexOfId(a) == 0 && tl.indexOfId(b) == 1 && tl.indexOfId(c) == 2);
    CHECK(lastDuration == 12000);

    // out-of-range insert index clamps
    const int d = tl.insertClip(999, "d.insv", 2000, 30.0);
    CHECK(tl.indexOfId(d) == 3);
    CHECK(lastDuration == 14000);

    // move d to the front
    CHECK(tl.moveClip(d, 0));
    CHECK(reordered == 1);
    CHECK(tl.indexOfId(d) == 0 && tl.indexOfId(a) == 1);
    CHECK(!tl.moveClip(d, 0));   // already there -> no-op, no signal
    CHECK(!tl.moveClip(999, 1)); // unknown id
    CHECK(reordered == 1);

    // playbackPlan reflects order + trims
    tl.setClipTrimIn(a, 500);
    CHECK(trimTouched == 1);
    const auto plan = tl.playbackPlan();
    CHECK(plan.size() == 4);
    CHECK(plan[0].sourcePath == "d.insv" && plan[0].trimInMs == 0);
    CHECK(plan[1].sourcePath == "a.insv" && plan[1].trimInMs == 500 && plan[1].trimOutMs == 4000);

    // remove
    CHECK(tl.removeClip(b));
    CHECK(removed == 1);
    CHECK(tl.count() == 3 && tl.indexOfId(b) < 0);
    CHECK(!tl.removeClip(b)); // gone already
    CHECK(removed == 1);

    // reframe signal + unknown-id safety
    tl.upsertClipReframeKey(a, 1000, {});
    CHECK(reframeTouched == 1);
    tl.setClipTrimIn(999, 1000);            // must not crash / signal
    tl.upsertClipReframeKey(999, 1000, {}); // must not crash / signal
    CHECK(trimTouched == 1 && reframeTouched == 1);
    tl.replaceClipReframeSpan(a, 0, 2000, {{500, {}}, {1500, {}}}); // one edit, one signal
    CHECK(reframeTouched == 2 && tl.clipById(a)->reframe.count() == 2);
    tl.replaceClipReframeSpan(999, 0, 2000, {}); // unknown id: no signal
    CHECK(reframeTouched == 2);

    // colour: all four fields set together, one signal, unknown-id safety
    int colorTouched = 0;
    ColorGrade lastGrade;
    QObject::connect(&tl, &Timeline::clipColorChanged, &tl, [&](int, ColorGrade g) {
        ++colorTouched;
        lastGrade = g;
    });
    ColorGrade grade;
    grade.exposureEv = 1.0f;
    grade.saturation = -50.0f;
    tl.setClipColor(a, grade);
    CHECK(colorTouched == 1);
    CHECK(lastGrade == grade);
    CHECK(tl.clipById(a)->color == grade);
    tl.setClipColor(a, grade); // no-op -> no signal
    CHECK(colorTouched == 1);
    tl.setClipColor(999, grade); // unknown id -- must not crash / signal
    CHECK(colorTouched == 1);

    tl.clear();
    CHECK(tl.isEmpty() && lastDuration == 0);
}

void testTimelineSplit()
{
    Timeline tl;
    const int a = tl.appendClip("a.insv", 20000, 30.0);
    tl.setClipTrimIn(a, 2000);
    tl.setClipTrimOut(a, 18000); // clip a spans source [2000, 18000)
    tl.upsertClipReframeKey(a, 5000, {0.5f, 0.0f, 0.0f, 1.2f});
    tl.upsertClipReframeKey(a, 15000, {1.0f, 0.0f, 0.0f, 1.0f});

    // invalid split points -> 0, nothing changes
    CHECK(tl.splitClip(a, 2000) == 0);        // at the in-point
    CHECK(tl.splitClip(a, 2050) == 0);        // inside the min gap
    CHECK(tl.splitClip(a, 18000) == 0);       // at the out-point
    CHECK(tl.splitClip(999, 10000) == 0);     // unknown id
    CHECK(tl.count() == 1);

    // valid split at source 10000
    const int b = tl.splitClip(a, 10000);
    CHECK(b != 0 && b != a);
    CHECK(tl.count() == 2);
    CHECK(tl.indexOfId(a) == 0 && tl.indexOfId(b) == 1);
    const ClipItem &left = tl.clipAt(0);
    const ClipItem &right = tl.clipAt(1);
    CHECK(left.trimInMs == 2000 && left.trimOutMs == 10000);
    CHECK(right.trimInMs == 10000 && right.trimOutMs == 18000);
    CHECK(right.sourcePath == left.sourcePath);
    // reframe track copied to both halves
    CHECK(left.reframe.count() == 2 && right.reframe.count() == 2);
    // total trimmed length unchanged by a split
    CHECK(tl.totalDurationMs() == 16000);
}

void writeFile(const QString &path, const QByteArray &data = QByteArrayLiteral("x"))
{
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(data);
    f.close();
}

// A clip's sound: each .insv stream on its stream track, linked to the clip.
void testClipSound()
{
    Timeline tl;
    const int a = tl.appendClip("/x/a.insv", 10000, 30.0, 0, 0, 1); // camera mic only
    CHECK(tl.trackCount() == 1);
    const int camId = tl.streamTrackId(Track::Role::CameraMic);
    CHECK(camId && !tl.streamTrackId(Track::Role::ExternalMic));
    if (!camId)
        return;
    CHECK(tl.trackById(camId)->name == QLatin1String("Camera mic"));
    CHECK(tl.trackById(camId)->clips.size() == 1);
    const TrackClip &ca = tl.trackById(camId)->clips.first();
    CHECK(ca.linkedClipId == a && ca.stream == 0 && ca.startMs == 0 && ca.trimOutMs == 10000);

    // A free track first, then a clip with an external mic: its track goes under
    // the camera mic's, above the free one.
    const int music = tl.addTrack(Track::Kind::Audio, "Music");
    const int b = tl.appendClip("/x/b.insv", 6000, 30.0, 0, 0, 2);
    const int airId = tl.streamTrackId(Track::Role::ExternalMic);
    CHECK(airId && tl.trackIndexOfId(camId) == 0 && tl.trackIndexOfId(airId) == 1 &&
          tl.trackIndexOfId(music) == 2);
    CHECK(tl.trackById(camId)->clips.size() == 2 && tl.trackById(airId)->clips.size() == 1);
    if (airId && tl.trackById(airId)->clips.size() == 1) {
        const TrackClip &air = tl.trackById(airId)->clips.first();
        CHECK(air.linkedClipId == b && air.stream == 0 && air.startMs == 10000);
    }
    CHECK(tl.trackById(camId)->clips[1].stream == 1); // the camera's own mic

    // Again for a stream that's already linked: nothing new.
    const QList<int> again = tl.addClipSound(b, 2);
    CHECK(again.size() == 2 && again[0] == 0 && again[1] == 0);

    // Stream tracks stay above the free ones, and among themselves.
    CHECK(!tl.moveTrack(music, 0));
    CHECK(!tl.moveTrack(camId, 2) || tl.trackIndexOfId(music) == 2);
    CHECK(tl.trackIndexOfId(music) == 2);

    // The sound follows its clip: a trim, a split, a move, a removal.
    tl.setClipTrimIn(a, 2000);
    CHECK(tl.trackById(camId)->clips.first().trimInMs == 2000);
    CHECK(tl.trackById(airId)->clips.first().startMs == 8000); // b moved up
    const int right = tl.splitClip(b, 3000);
    CHECK(right && tl.trackById(airId)->clips.size() == 2);
    CHECK(tl.moveClip(a, 2));
    CHECK(tl.trackById(camId)->clips.last().linkedClipId == a);
    CHECK(tl.removeClip(b) && tl.trackById(airId)->clips.size() == 1);

    // Nothing for a file without sound.
    Timeline silent;
    silent.appendClip("/x/s.insv", 1000, 30.0, 0, 0, 0);
    CHECK(silent.trackCount() == 0);
}

// A project from before version 10: each clip played one stream itself; on
// load every stream goes on its stream track, the picked one with the clip's
// level / mute / noise (and the main "M"), the others muted; a mic put on a
// track by hand moves to its stream track.
void testProjectConvertSound()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString clip = tmp.path() + "/two.insv";
    writeFile(clip);
    const QString orp = tmp.path() + "/old.orp";
    writeFile(orp, QStringLiteral(R"({"format": "orbis-reframe-project", "version": 9,
        "timeline": {"mainMuted": true, "clips": [
          {"source": "two.insv", "trimInMs": 0, "trimOutMs": 5000, "sourceDurationMs": 5000,
           "fps": 30, "audioGainDb": -4, "audioMuted": false, "audioTrack": 1,
           "noise": {"mode": "wind", "strength": 0.5}},
          {"source": "gone.insv", "trimInMs": 0, "trimOutMs": 3000, "sourceDurationMs": 3000,
           "fps": 30, "audioTrack": 0}],
         "tracks": [{"kind": "audio", "name": "Mics", "gainDb": 0, "muted": false, "clips": [
           {"source": "two.insv", "stream": 0, "sourceDurationMs": 5000, "startMs": 0,
            "trimInMs": 0, "trimOutMs": 5000, "speed": 1, "gainDb": -2, "muted": false,
            "fadeInMs": 0, "fadeOutMs": 0, "linkedClip": 0}]}]}})")
                       .toUtf8());

    Project p;
    p.setAudioStreamCounter([&](const QString &path) { return path == clip ? 2 : 0; });
    QString err;
    CHECK(p.load(orp, &err));
    Timeline *tl = p.timeline();
    const int camId = tl->streamTrackId(Track::Role::CameraMic);
    const int extId = tl->streamTrackId(Track::Role::ExternalMic);
    CHECK(camId && extId);
    if (!camId || !extId)
        return;
    // Camera mic, External mic, then the free tracks; "Mics" is gone (its
    // one clip moved).
    CHECK(tl->trackIndexOfId(camId) == 0 && tl->trackIndexOfId(extId) == 1);
    CHECK(tl->trackCount() == 2);
    const Track &cam = *tl->trackById(camId);
    const Track &ext = *tl->trackById(extId);
    CHECK(cam.name == QLatin1String("Camera mic") && !cam.muted);
    CHECK(ext.name == QLatin1String("External mic") && !ext.muted);
    CHECK(cam.clips.size() == 2);
    if (cam.clips.size() == 2) {
        const TrackClip &m = cam.clips[0]; // the picked stream: the camera's own mic
        CHECK(m.stream == 1 && m.linkedClipId == tl->clipAt(0).id);
        // Its level and noise, muted by the video track's old "M".
        CHECK(m.gainDb == -4.0 && m.muted && m.noise.mode == insv::NoiseMode::MotorcycleWind);
        // The missing file: as many streams as its picked one needs.
        CHECK(cam.clips[1].stream == 0 && cam.clips[1].linkedClipId == tl->clipAt(1).id);
    }
    // The external mic put on "Mics" by hand moved here, its level kept.
    CHECK(ext.clips.size() == 1);
    if (ext.clips.size() == 1) {
        CHECK(ext.clips.first().stream == 0 && ext.clips.first().gainDb == -2.0);
        CHECK(!ext.clips.first().muted && ext.clips.first().linkedClipId == tl->clipAt(0).id);
    }
    CHECK(!p.modified()); // a load, not an edit

    // Saved again, it's a version-10 file with the roles.
    CHECK(p.save(orp, &err));
    QFile f(orp);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray text = f.readAll();
    CHECK(text.contains("\"version\":10") && text.contains("\"role\":\"cameraMic\"") &&
          text.contains("\"role\":\"externalMic\""));
    CHECK(!text.contains("audioTrack") && !text.contains("mainMuted"));
}

void testStabConfig()
{
    // Lean-follow Strength (0..1) <-> shared smoothing window (s), the mapping
    // the Stabilization panel's slider rides on.
    CHECK(insv::leanWindowFromStrength(0.0f) < insv::leanWindowFromStrength(0.5f));
    CHECK(insv::leanWindowFromStrength(0.5f) < insv::leanWindowFromStrength(1.0f));
    // 0.5 reproduces the historical ~0.9 s LeanFollow default.
    CHECK(std::fabs(insv::leanWindowFromStrength(0.5f) - 0.9f) < 0.05f);
    CHECK(insv::StabConfig::preset(insv::StabMode::LeanFollow).windowRollSec ==
          insv::leanWindowFromStrength(0.5f));
    // mutually inverse in the interior
    for (float s : {0.1f, 0.35f, 0.6f, 0.9f}) {
        const float back = insv::leanStrengthFromWindow(insv::leanWindowFromStrength(s));
        CHECK(std::fabs(back - s) < 1e-3f);
    }
    // clamp-safe both directions
    CHECK(insv::leanWindowFromStrength(-1.0f) == insv::leanWindowFromStrength(0.0f));
    CHECK(insv::leanWindowFromStrength(2.0f) == insv::leanWindowFromStrength(1.0f));
    CHECK(insv::leanStrengthFromWindow(0.01f) == 0.0f);
    CHECK(insv::leanStrengthFromWindow(99.0f) == 1.0f);
    CHECK(std::fabs(insv::leanWindowFromStrength(1.0f) - 5.0f) < 1e-4f); // heavy-smoothing end

    // Horizon-lock Smoothing <-> heading window: 0.3..10 s, 0.5 == the preset.
    CHECK(std::fabs(insv::horizonWindowFromStrength(0.0f) - 0.3f) < 1e-4f);
    CHECK(std::fabs(insv::horizonWindowFromStrength(1.0f) - 10.0f) < 1e-3f);
    CHECK(std::fabs(insv::horizonWindowFromStrength(0.5f) -
                    insv::StabConfig::preset(insv::StabMode::HorizonLock).windowYawSec) < 1e-3f);
    for (float s : {0.0f, 0.2f, 0.5f, 0.8f, 1.0f})
        CHECK(std::fabs(insv::horizonStrengthFromWindow(insv::horizonWindowFromStrength(s)) - s) <
              1e-3f);
    CHECK(insv::horizonWindowFromStrength(0.3f) < insv::horizonWindowFromStrength(0.7f));

    // build(InsvTelemetry) fits the frame clock's rate against the nominal
    // fps from the exposure record. The offset targets the centre sensor
    // row (-2.3 ms), and the readout (field 25) is reported for the stitch.
    // Synthetic: 1 kHz IMU from device t = 1 s, frames on a +40 ppm clock with
    // 5 pre-roll exposures before field 24's frame, 20 ms readout.
    {
        const double fps = 30000.0 / 1001.0;
        insv::InsvTelemetry tele;
        for (int i = 0; i < 700000; ++i) { // ~700 s
            insv::ImuSample s;
            s.timestamp = 1000000 + static_cast<uint64_t>(i) * 1000; // us
            s.accel[1] = 1.0;
            tele.imu.push_back(s);
        }
        const double rate = 40e-6;
        const uint64_t f24 = 1500000; // 0.5 s after IMU[0]
        for (int n = -5; n < 20000; ++n) {
            insv::ExposureSample e;
            e.timestamp = static_cast<uint64_t>(
                static_cast<int64_t>(f24) + std::llround(n / fps * (1.0 + rate) * 1e6));
            tele.exposure.push_back(e);
        }
        tele.first_frame_timestamp = f24;
        tele.gyro_timestamp_ms = 1.6;
        tele.rolling_shutter_ms = 20.0;

        insv::GyroStabilizer g;
        g.build(tele, fps, insv::StabConfig::preset(insv::StabMode::LeanFollow));
        CHECK(g.ready());
        CHECK(std::fabs(g.clockRatePpm() - 40.0) < 0.5);
        // 0.5 s pre-roll + 1.6 ms IMU latency - 2.3 ms to the centre sensor row
        CHECK(std::fabs(g.effectiveTimeOffsetSec() - 0.4993f) < 1e-4f);
        CHECK(std::fabs(g.readoutSec() - 0.020) < 1e-12);
        // A measured fine-tune adds to the derived offset.
        g.build(tele, fps, insv::StabConfig::preset(insv::StabMode::LeanFollow), 0.003);
        CHECK(std::fabs(g.effectiveTimeOffsetSec() - 0.5023f) < 1e-4f);

        // Explicit override: offset kept as-is, clock rate still fitted.
        insv::StabConfig ov = insv::StabConfig::preset(insv::StabMode::LeanFollow);
        ov.timeOffsetSec = 0.25f;
        g.build(tele, fps, ov);
        CHECK(g.effectiveTimeOffsetSec() == 0.25f);
        CHECK(std::fabs(g.clockRatePpm() - 40.0) < 0.5);

        // The plain IMU overload knows nothing about frames: nominal time.
        g.build(tele.imu, insv::StabConfig::preset(insv::StabMode::LeanFollow));
        CHECK(g.clockRatePpm() == 0.0);
    }

    // bodyRotation(): the camera turn in the axis-mapped body frame, for the
    // stitch RS correction. A constant 1 rad/s about raw gyro z lands on body X
    // under the default "z,-x,-y" map (no accel -> pure gyro integration).
    {
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 3000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000; // us, 1 kHz
            s.gyro[2] = 1.0;
            imu.push_back(s);
        }
        insv::GyroStabilizer g;
        g.build(imu, insv::StabConfig::preset(insv::StabMode::LeanFollow));
        CHECK(g.ready());
        float r[3];
        g.bodyRotation(1.5, 0.01, r); // 10 ms at 1 rad/s
        CHECK(std::fabs(r[0] - 0.01f) < 1e-5f);
        CHECK(std::fabs(r[1]) < 1e-5f && std::fabs(r[2]) < 1e-5f);
        g.bodyRotation(1.5, -0.01, r);
        CHECK(std::fabs(r[0] + 0.01f) < 1e-5f);
    }

    // 2026-09-25: LeanFollow is pure gyro -- accelerometer junk (a 0.4 g 11 Hz
    // stick resonance) must not move its correction at all. Auto-level (on by
    // default) only reads the clip's mean accel, a constant ~identity here;
    // with it off the correction is exactly identity.
    {
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 3000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000; // us, 1 kHz
            const double n = 2.0 * std::numbers::pi * 11.0 * i * 1e-3;
            s.accel[0] = -1.0;
            s.accel[1] = 0.4 * std::sin(n);
            s.accel[2] = 0.4 * std::sin(n + 1.0);
            imu.push_back(s);
        }
        for (const bool autoLevel : {true, false}) {
            insv::StabConfig lc = insv::StabConfig::preset(insv::StabMode::LeanFollow);
            lc.autoLevel = autoLevel;
            insv::GyroStabilizer g;
            g.build(imu, lc);
            CHECK(g.ready());
            float m0[3][3];
            g.correctionMatrix(0.8, m0);
            float worst = 0.0f, offIdentity = 0.0f;
            for (int k = 0; k < 30; ++k) {
                float m[3][3];
                g.correctionMatrix(0.8 + k / 30.0, m);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) {
                        worst = std::max(worst, std::fabs(m[r][c] - m0[r][c]));
                        offIdentity = std::max(offIdentity,
                                               std::fabs(m[r][c] - (r == c ? 1.0f : 0.0f)));
                    }
            }
            CHECK(worst < 1e-5f);
            CHECK(offIdentity < (autoLevel ? 1e-3f : 1e-5f));
        }
    }

    // Horizon lock levels a tilted mount: gravity-up is 20 deg off body +Y
    // (a rolled camera), gyro still. The correction must carry the view's up
    // onto that measured up, and the view's forward must be horizontal.
    {
        const double a = 20.0 * std::numbers::pi / 180.0;
        const double b[3] = {std::sin(a), std::cos(a), 0.0}; // body up
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 3000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            // default map "z,-x,-y": body X <- z, Y <- -x, Z <- -y
            s.accel[2] = b[0];
            s.accel[0] = -b[1];
            s.accel[1] = -b[2];
            imu.push_back(s);
        }
        insv::GyroStabilizer g;
        g.build(imu, insv::StabConfig::preset(insv::StabMode::HorizonLock));
        CHECK(g.ready());
        float m[3][3];
        g.correctionMatrix(1.5, m);
        const float up[3] = {m[0][1], m[1][1], m[2][1]};     // C * (0,1,0)
        const float fwd[3] = {-m[0][2], -m[1][2], -m[2][2]}; // C * (0,0,-1)
        CHECK(std::fabs(up[0] - b[0]) < 1e-3f && std::fabs(up[1] - b[1]) < 1e-3f &&
              std::fabs(up[2] - b[2]) < 1e-3f);
        CHECK(std::fabs(fwd[0] * b[0] + fwd[1] * b[1] + fwd[2] * b[2]) < 1e-3f);
    }

    // 2026-10-09: a residual gyro bias (the X5 keeps ~0.3-0.4 deg/s) against
    // the weak accel anchor used to leave Horizon lock several degrees off
    // level. Still camera, gravity 20 deg off body +Y, 0.006 rad/s of bias on
    // the two axes across gravity: the whole-clip fit must level it.
    {
        const double a = 20.0 * std::numbers::pi / 180.0;
        const double b[3] = {std::sin(a), std::cos(a), 0.0}; // body up
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 20000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            s.accel[2] = b[0]; // default map "z,-x,-y"
            s.accel[0] = -b[1];
            s.accel[1] = -b[2];
            s.gyro[1] = -0.006; // body Z (forward axis): rolls the horizon
            s.gyro[2] = 0.004;  // body X: pitches it
            imu.push_back(s);
        }
        insv::GyroStabilizer g;
        g.build(imu, insv::StabConfig::preset(insv::StabMode::HorizonLock));
        CHECK(g.ready());
        double worst = 0.0;
        for (double t : {0.5, 5.0, 10.0, 19.5}) {
            float m[3][3];
            g.correctionMatrix(t, m);
            const double dot = m[0][1] * b[0] + m[1][1] * b[1] + m[2][1] * b[2];
            worst = std::max(worst, std::acos(std::min(1.0, dot)) * 180.0 / std::numbers::pi);
        }
        CHECK(worst < 0.2);
    }

    // Lean follow auto-level: a still camera on a mount tilted 20 deg. On, the
    // view's up is the mount's gravity up; off, the camera body's own up.
    {
        const double a = 20.0 * std::numbers::pi / 180.0;
        const double b[3] = {std::sin(a), std::cos(a), 0.0}; // body up
        std::vector<insv::ImuSample> imu;
        for (int i = 0; i < 3000; ++i) {
            insv::ImuSample s;
            s.timestamp = static_cast<uint64_t>(i) * 1000;
            s.accel[2] = b[0];
            s.accel[0] = -b[1];
            s.accel[1] = -b[2];
            imu.push_back(s);
        }
        insv::StabConfig lc = insv::StabConfig::preset(insv::StabMode::LeanFollow);
        CHECK(lc.autoLevel);
        insv::GyroStabilizer g;
        g.build(imu, lc);
        float m[3][3];
        g.correctionMatrix(1.5, m);
        CHECK(std::fabs(m[0][1] - b[0]) < 1e-3f && std::fabs(m[1][1] - b[1]) < 1e-3f &&
              std::fabs(m[2][1] - b[2]) < 1e-3f);
        lc.autoLevel = false;
        g.build(imu, lc);
        g.correctionMatrix(1.5, m);
        CHECK(std::fabs(m[0][1]) < 1e-5f && std::fabs(m[1][1] - 1.0f) < 1e-5f);
    }

    // A LeanFollow config with a retuned (non-default) window round-trips
    // through the .orp like any other StabConfig.
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString projFile = tmp.path() + "/lean.orp";
    insv::StabConfig lean = insv::StabConfig::preset(insv::StabMode::LeanFollow);
    lean.windowYawSec = lean.windowPitchSec = lean.windowRollSec =
        insv::leanWindowFromStrength(0.8f);
    {
        Project p;
        const int id = p.timeline()->appendClip("a.insv", 1000, 30.0);
        p.timeline()->setClipStabilization(id, lean);
        CHECK(p.timeline()->clipAt(0).stabilization == lean);
        QString err;
        CHECK(p.save(projFile, &err));
    }
    {
        Project p;
        QString err;
        CHECK(p.load(projFile, &err));
        CHECK(p.timeline()->count() == 1);
        const insv::StabConfig &st = p.timeline()->clipAt(0).stabilization;
        CHECK(st.mode == insv::StabMode::LeanFollow);
        CHECK(std::fabs(st.windowRollSec - insv::leanWindowFromStrength(0.8f)) < 1e-4f);
        // the slider position the panel would show on load
        CHECK(std::fabs(insv::leanStrengthFromWindow(st.windowRollSec) - 0.8f) < 1e-3f);
    }
}

void testProjectJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString projDir = tmp.path();
    QDir().mkpath(projDir + "/media");
    const QString insideSrc = QDir::cleanPath(projDir + "/media/clip_a.insv");
    writeFile(insideSrc);

    QTemporaryDir tmp2; // a source that lives outside the project dir
    CHECK(tmp2.isValid());
    const QString outsideSrc = QDir::cleanPath(tmp2.path() + "/clip_b.insv");
    writeFile(outsideSrc);

    const QString projFile = projDir + "/show.orp";

    // build + save
    {
        Project p;
        CHECK(!p.modified());
        const int a = p.timeline()->appendClip(insideSrc, 10000, 30.0, 1920, 960, 2);
        p.timeline()->appendClip(outsideSrc, 6000, 25.0, 0, 0, 1);
        CHECK(p.modified()); // an edit dirtied it
        p.timeline()->setClipTrimIn(a, 1000);
        p.timeline()->setClipTrimOut(a, 8000);
        p.timeline()->upsertClipReframeKey(a, 2000, {0.5f, -0.25f, 0.1f, 1.1f});
        p.timeline()->upsertClipReframeKey(a, 5000, {1.5f, 0.0f, 0.0f, 0.9f});
        { // Audio panel: the camera mic's clip (stream 1 of 2)
            const Track *cam = p.timeline()->trackById(
                p.timeline()->streamTrackId(Track::Role::CameraMic));
            CHECK(cam && cam->clips.size() == 2);
            if (cam && !cam->clips.isEmpty()) {
                const int mic = cam->clips.first().id;
                p.timeline()->setAudioClipLevel(mic, -4.5, true);
                p.timeline()->setAudioClipNoise(mic, {insv::NoiseMode::MotorcycleWind, 0.5f});
            }
        }
        ColorGrade grade;
        grade.exposureEv = -1.5f;
        grade.contrast = 25.0f;
        grade.saturation = -30.0f;
        grade.temperature = 15.0f;
        p.timeline()->setClipColor(a, grade); // Colour panel
        p.timeline()->setClipBlurPlates(a, true); // Privacy panel
        {
            privacy::BlurRemoval rm; // a blur track removed by hand
            rm.fromMs = 3000;
            rm.toMs = 3100;
            rm.wholeTrack = false;
            rm.path = {{3000, {0.0f, -0.6f, 0.8f}}, {3100, {0.1f, -0.6f, 0.79f}}};
            p.timeline()->setClipBlurRemovals(a, {rm});
            privacy::BlurAddition add; // a plate blurred by hand
            privacy::BlurAddition::Frame f;
            f.tMs = 4000;
            f.kind = 2;
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i < 3; ++i)
                    f.corner[k][i] = 0.1f * k + 0.01f * i;
            add.frames = {f};
            f.tMs = 4033;
            add.frames.push_back(f);
            add.timing = privacy::StitchTiming::Uncorrected; // followed with the stabilization off
            p.timeline()->setClipBlurAdditions(a, {add});
        }
        p.timeline()->setClipSpeed(a, 0.5);
        p.timeline()->setSourceSyncAdjust(insideSrc, -3.1); // sync fine-tune
        p.timeline()->setSourceSeamAdjust(insideSrc, {-0.55, 0.05, -0.05}, 2); // seam fine-tune
        CHECK(p.modified());

        stitch::PreviewStitchConfig sc;
        sc.quality = stitch::PreviewStitchConfig::Quality::OpticalFlow;
        sc.width = 2560;
        sc.height = 1280;
        sc.lensGuard = stitch::PreviewStitchConfig::LensGuard::Standard;
        sc.depthSeam = false; // default on
        p.setPreviewStitch(sc);
        CHECK(p.previewStitch().width == 2560);
        CHECK(p.previewStitch() == sc);
        CHECK(sc.cacheTag().find("2560x1280") != std::string::npos);

        insv::StabConfig stab = insv::StabConfig::preset(insv::StabMode::Custom);
        stab.windowYawSec = 0.7f;
        stab.windowRollSec = 1.3f;
        stab.horizonLockAmount = 0.25f;
        stab.horizonGain = 0.6f;
        stab.rollOffsetRad = 0.05f;
        stab.accelLowPassHz = 1.5f;
        stab.autoLevel = false;
        p.timeline()->setClipStabilization(a, stab); // Stabilization panel
        CHECK(p.timeline()->clipById(a)->stabilization == stab);
        CHECK(p.timeline()->clipAt(1).stabilization == insv::StabConfig{}); // per clip

        OutputSettings out; // Project Settings panel
        out.aspect = OutputSettings::Aspect::Portrait9x16;
        out.width = 1080;
        out.height = 1920;
        out.fps = 25.0;
        p.setOutput(out);
        CHECK(p.output() == out);
        p.timeline()->setTrackAudio(p.timeline()->streamTrackId(Track::Role::ExternalMic), 0.0,
                                    true); // the external mic track's "M"

        QString err;
        CHECK(p.save(projFile, &err));
        CHECK(err.isEmpty());
        CHECK(!p.modified());           // save clears the flag
        CHECK(p.filePath() == projFile);
    }

    // in-dir source stored relative, out-of-dir source stored absolute
    {
        QFile f(projFile);
        CHECK(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        CHECK(text.contains(QStringLiteral("\"media/clip_a.insv\"")));
        CHECK(text.contains(QStringLiteral("\"%1\"").arg(outsideSrc)));
    }

    // load into a fresh project and compare
    {
        Project p;
        QString err;
        CHECK(p.load(projFile, &err));
        CHECK(err.isEmpty());
        CHECK(!p.modified());
        CHECK(p.filePath() == projFile);

        Timeline *tl = p.timeline();
        CHECK(tl->count() == 2);

        const ClipItem &a = tl->clipAt(0);
        CHECK(a.sourcePath == insideSrc); // relative -> resolved back to abs
        CHECK(!a.mediaMissing);
        CHECK(a.sourceDurationMs == 10000);
        CHECK(a.trimInMs == 1000 && a.trimOutMs == 8000);
        CHECK(a.sourceWidth == 1920 && a.sourceHeight == 960);
        CHECK(a.reframe.count() == 2);
        CHECK(a.reframe.keys()[0].timeMs == 2000);
        CHECK(a.reframe.keys()[0].pose.yaw > 0.49f && a.reframe.keys()[0].pose.yaw < 0.51f);
        CHECK(a.reframe.keys()[0].pose.roll > 0.09f && a.reframe.keys()[0].pose.roll < 0.11f);
        CHECK(a.reframe.keys()[1].pose.roll == 0.0f); // key 2 authored with roll 0
        CHECK(a.reframe.keys()[1].pose.fovY > 0.89f && a.reframe.keys()[1].pose.fovY < 0.91f);
        // The stream tracks round-trip: roles, order, mute, the clips' links
        // and the Audio panel's level / noise.
        const int camId = tl->streamTrackId(Track::Role::CameraMic);
        const int airId = tl->streamTrackId(Track::Role::ExternalMic);
        CHECK(camId && airId && tl->trackIndexOfId(camId) == 0 && tl->trackIndexOfId(airId) == 1);
        if (camId && airId) {
            const Track &cam = *tl->trackById(camId);
            const Track &air = *tl->trackById(airId);
            CHECK(cam.name == QLatin1String("Camera mic") && !cam.muted);
            CHECK(air.name == QLatin1String("External mic") && air.muted && air.clips.size() == 1);
            CHECK(cam.clips.size() == 2);
            if (cam.clips.size() == 2) {
                const TrackClip &m = cam.clips[0];
                CHECK(m.linkedClipId == a.id && m.stream == 1);
                CHECK(m.gainDb == -4.5 && m.muted);
                CHECK(m.noise.mode == insv::NoiseMode::MotorcycleWind && m.noise.strength == 0.5f);
                CHECK(cam.clips[1].linkedClipId == tl->clipAt(1).id && cam.clips[1].stream == 0);
                CHECK(cam.clips[1].noise == insv::NoiseReduction{} && !cam.clips[1].muted);
            }
        }
        CHECK(a.speed == 0.5);
        CHECK(a.color.exposureEv == -1.5f && a.color.contrast == 25.0f); // Colour panel
        CHECK(a.color.saturation == -30.0f && a.color.temperature == 15.0f);

        const ClipItem &b = tl->clipAt(1);
        CHECK(b.sourcePath == outsideSrc);
        CHECK(b.trimOutMs == 6000);
        CHECK(b.color == ColorGrade{}); // never touched -- defaults
        CHECK(a.blurPlates && !b.blurPlates); // privacy blur
        CHECK(a.blurRemovals.size() == 1 && b.blurRemovals.empty());
        CHECK(a.blurAdditions.size() == 1 && b.blurAdditions.empty());
        if (a.blurAdditions.size() == 1) {
            const auto &fr = a.blurAdditions[0].frames;
            CHECK(fr.size() == 2 && fr[0].tMs == 4000 && fr[1].tMs == 4033 && fr[0].kind == 2);
            CHECK(fr.size() == 2 && std::abs(fr[1].corner[3][2] - (0.1f * 3 + 0.01f * 2)) < 1e-6f);
            CHECK(a.blurAdditions[0].timing == privacy::StitchTiming::Uncorrected);
        }
        if (a.blurRemovals.size() == 1) {
            const privacy::BlurRemoval &rm = a.blurRemovals[0];
            CHECK(rm.fromMs == 3000 && rm.toMs == 3100 && !rm.wholeTrack && rm.path.size() == 2);
            CHECK(rm.path.size() == 2 && rm.path[1].tMs == 3100 && rm.path[1].dir[0] == 0.1f &&
                  rm.path[1].dir[2] == 0.79f);
        }
        CHECK(a.syncAdjustMs > -3.11 && a.syncAdjustMs < -3.09); // round-trips
        CHECK(std::isnan(b.syncAdjustMs));                        // never measured
        // A new clip cut from a measured source inherits its fine-tune.
        const int a2 = tl->appendClip(insideSrc, 10000, 29.97, 1920, 960);
        CHECK(tl->clipById(a2)->syncAdjustMs == a.syncAdjustMs);
        CHECK(tl->clipById(a2)->seamAdjustGuard == 2);
        CHECK(tl->clipById(a2)->seamAdjustDeg[0] == -0.55);

        const stitch::PreviewStitchConfig &sc = p.previewStitch();
        CHECK(sc.quality == stitch::PreviewStitchConfig::Quality::OpticalFlow);
        CHECK(sc.width == 2560 && sc.height == 1280);
        CHECK(sc.lensGuard == stitch::PreviewStitchConfig::LensGuard::Standard);
        CHECK(!sc.depthSeam);

        CHECK(tl->clipAt(1).stabilization == insv::StabConfig{}); // never touched
        const insv::StabConfig &st = tl->clipAt(0).stabilization; // round-trips
        CHECK(st.mode == insv::StabMode::Custom);
        CHECK(st.windowYawSec > 0.69f && st.windowYawSec < 0.71f);
        CHECK(st.windowRollSec > 1.29f && st.windowRollSec < 1.31f);
        CHECK(st.horizonLockAmount > 0.24f && st.horizonLockAmount < 0.26f);
        CHECK(st.horizonGain > 0.59f && st.horizonGain < 0.61f); // round-trips
        CHECK(st.rollOffsetRad > 0.04f && st.rollOffsetRad < 0.06f);
        CHECK(st.accelLowPassHz > 1.49f && st.accelLowPassHz < 1.51f);
        CHECK(!st.autoLevel);

        const OutputSettings &out = p.output(); // Project Settings round-trips
        CHECK(out.aspect == OutputSettings::Aspect::Portrait9x16);
        CHECK(out.width == 1080 && out.height == 1920);
        CHECK(out.fps == 25.0);
        CHECK(out.effectiveFps(29.97) == 25.0);

    }

    // missing blocks load with defaults
    {
        const QString bare = projDir + "/bare.orp";
        writeFile(bare, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":8,"timeline":{"clips":[]}})"));
        Project p;
        QString err;
        CHECK(p.load(bare, &err));
        CHECK(p.previewStitch() == stitch::PreviewStitchConfig{});
        CHECK(p.output() == OutputSettings{});
        CHECK(p.output().fps == 0.0 && p.output().effectiveFps(29.97) == 29.97);
    }

    // a hand-broken "output" block falls back to the aspect's default size
    {
        const QString badSize = projDir + "/badsize.orp";
        writeFile(badSize, QByteArrayLiteral(
                               R"({"format":"orbis-reframe-project","version":8,"output":{"aspect":"1:1","width":0,"height":-5,"fps":-3},"timeline":{"clips":[]}})"));
        Project p;
        QString err;
        CHECK(p.load(badSize, &err));
        CHECK(p.output().aspect == OutputSettings::Aspect::Square1x1);
        CHECK(p.output().width == 1080 && p.output().height == 1080);
        CHECK(p.output().fps == 0.0);
    }

    // missing media: still loads, flags just that clip
    {
        QFile::remove(insideSrc);
        Project p;
        QString err;
        CHECK(p.load(projFile, &err)); // non-fatal
        CHECK(p.timeline()->clipAt(0).mediaMissing);
        CHECK(!p.timeline()->clipAt(1).mediaMissing);
    }

    // resetWithClip clears the file path and the modified flag
    {
        Project p;
        QString err;
        CHECK(p.load(projFile, &err));
        p.timeline()->setClipTrimIn(p.timeline()->clipAt(1).id, 500);
        CHECK(p.modified());
        const int id = p.resetWithClip(outsideSrc, 4000, 30.0);
        CHECK(p.timeline()->count() == 1);
        CHECK(p.timeline()->clipById(id) != nullptr);
        CHECK(p.filePath().isEmpty());
        CHECK(!p.modified());
    }

    // a malformed / foreign / older / newer file is rejected, model left untouched
    {
        Project p;
        p.timeline()->appendClip("keepme.insv", 1000, 30.0);
        const int before = p.timeline()->count();
        const QString junk = projDir + "/bad.orp";
        QString err;

        writeFile(junk, QByteArrayLiteral("{ not json ]"));
        CHECK(!p.load(junk, &err));
        CHECK(!err.isEmpty());
        CHECK(p.timeline()->count() == before);

        writeFile(junk, QByteArrayLiteral(R"({"format":"something-else","version":1})"));
        CHECK(!p.load(junk, &err));

        writeFile(junk, QByteArrayLiteral(
                            R"({"format":"insta-editor-project","version":8,"timeline":{"clips":[]}})"));
        CHECK(!p.load(junk, &err));

        writeFile(junk, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":7,"timeline":{"clips":[]}})"));
        CHECK(!p.load(junk, &err));
        CHECK(err.contains("version 7"));

        writeFile(junk, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":999,"timeline":{"clips":[]}})"));
        CHECK(!p.load(junk, &err));

        CHECK(p.timeline()->count() == before);

        // a clip without a frame rate
        writeFile(junk, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":8,"timeline":{"clips":[{"source":"a.insv","sourceDurationMs":1000}]}})"));
        CHECK(!p.load(junk, &err));
        CHECK(err.contains("frame rate"));
        CHECK(p.timeline()->count() == before);
    }
}

void testProjectUndoRedo()
{
    // No QCoreApplication in this binary, so Project's coalescing QTimer
    // never actually fires (there's no event loop to pump). undo()/redo()
    // flush any pending edit themselves (Project::commitPendingUndoPoint()),
    // so this exercises exactly the documented behaviour (Project.h): edits
    // made back-to-back with no undo()/redo() in between coalesce into one
    // step; calling undo()/redo() settles whatever came before it, so the
    // next edit starts its own, separate step.
    Project p;
    CHECK(!p.canUndo() && !p.canRedo());

    // Two edits back-to-back (no undo()/redo() between them) coalesce into
    // one step: appendClip immediately followed by a trim.
    const int a = p.timeline()->appendClip("a.insv", 10000, 30.0);
    p.timeline()->setClipTrimIn(a, 2000);
    CHECK(p.timeline()->count() == 1 && p.timeline()->clipAt(0).trimInMs == 2000);
    CHECK(p.canUndo() && !p.canRedo());

    p.undo(); // one step back past *both* edits -- they were never settled apart
    CHECK(p.timeline()->count() == 0);
    CHECK(!p.canUndo() && p.canRedo());

    p.redo(); // brings back the clip *and* the trim together
    CHECK(p.timeline()->count() == 1);
    CHECK(p.timeline()->clipAt(0).sourcePath == "a.insv");
    CHECK(p.timeline()->clipAt(0).trimInMs == 2000);
    CHECK(p.canUndo() && !p.canRedo());

    // A later edit is a genuinely separate step: undo() only reverts it,
    // leaving the earlier (already-settled) append+trim in place.
    const int idAfterRedo = p.timeline()->clipAt(0).id; // undo()/redo() rebuild -> fresh id
    ColorGrade warm;
    warm.exposureEv = 1.0f;
    p.timeline()->setClipColor(idAfterRedo, warm);
    p.undo();
    CHECK(p.timeline()->clipAt(0).color == ColorGrade{}); // just the colour edit undone
    CHECK(p.timeline()->clipAt(0).trimInMs == 2000);       // the earlier step survives
    CHECK(p.canUndo() && p.canRedo()); // one more step (append+trim) still behind us

    p.undo(); // back past the append+trim step too
    CHECK(p.timeline()->count() == 0);
    CHECK(!p.canUndo() && p.canRedo());

    // A fresh edit after undo() drops the redo history immediately.
    p.timeline()->appendClip("b.insv", 5000, 30.0);
    CHECK(!p.canRedo());
    CHECK(p.canUndo());

    // An output-settings change is its own undo step (Project Settings).
    p.undo(); // settle: back to the empty timeline
    OutputSettings sq;
    sq.aspect = OutputSettings::Aspect::Square1x1;
    sq.width = sq.height = 1080;
    p.setOutput(sq);
    CHECK(p.canUndo());
    p.undo();
    CHECK(p.output() == OutputSettings{});
    p.redo();
    CHECK(p.output() == sq);

    // newProject() drops all undo history.
    p.newProject();
    CHECK(!p.canUndo() && !p.canRedo());
    CHECK(p.output() == OutputSettings{});
}

void testExportBitrate()
{
    using Q = OutputSettings::Quality;
    // Standard: 40 Mbit/s at 1080p30 H.264, whole Mbit/s.
    CHECK(OutputSettings::bitrateKbpsFor(1920, 1080, 30.0, false) == 40000);
    // H.265 needs less; a higher frame rate more, though not twice as much.
    CHECK(OutputSettings::bitrateKbpsFor(1920, 1080, 30.0, true) == 28000);
    const int64_t at60 = OutputSettings::bitrateKbpsFor(1920, 1080, 60.0, false);
    CHECK(at60 > 40000 && at60 < 80000);
    // fps 0 (unknown) counts as 30.
    CHECK(OutputSettings::bitrateKbpsFor(1920, 1080, 0.0, false) == 40000);
    // The levels around Standard; Custom falls back to it.
    CHECK(OutputSettings::bitrateKbpsFor(1920, 1080, 30.0, false, Q::Smaller) == 20000);
    CHECK(OutputSettings::bitrateKbpsFor(1920, 1080, 30.0, false, Q::High) == 80000);
    CHECK(OutputSettings::bitrateKbpsFor(1920, 1080, 30.0, false, Q::Custom) == 40000);
    // Clamped to what the encoder accepts.
    CHECK(OutputSettings::bitrateKbpsFor(3840, 2160, 60.0, false, Q::High) ==
          OutputSettings::kMaxBitrateKbps);
    CHECK(OutputSettings::bitrateKbpsFor(16, 16, 1.0, true, Q::Smaller) ==
          OutputSettings::kMinBitrateKbps);
}

void testAudioTracks()
{
    Timeline tl;
    int changes = 0;
    QObject::connect(&tl, &Timeline::tracksChanged, [&] { ++changes; });
    const int v = tl.appendClip("main.insv", 20000, 30.0);

    const int t1 = tl.addTrack(Track::Kind::Audio);
    const int t2 = tl.addTrack(Track::Kind::Audio, "Mics");
    CHECK(tl.trackCount() == 2 && changes == 2);
    CHECK(tl.trackAt(0).name == "Audio 1" && tl.trackAt(1).name == "Mics");

    // Ids are timeline-unique: an audio clip never shares one with a main clip.
    const int m = tl.addAudioClip(t1, "song.mp3", 60000, 5000);
    CHECK(m != 0 && m != v);
    CHECK(tl.addAudioClip(999, "x.mp3", 1000, 0) == 0); // no such track
    int onTrack = 0;
    const TrackClip *c = tl.trackClipById(m, &onTrack);
    CHECK(c && onTrack == t1);
    CHECK(c->trimInMs == 0 && c->trimOutMs == 60000 && c->endMs() == 65000);
    CHECK(tl.extraTracksEndMs() == 65000);
    CHECK(tl.totalDurationMs() == 20000); // the main track alone sets the length

    // Head trim keeps the remaining audio in place on the timeline.
    tl.setTrackClipTrimIn(m, 2000);
    c = tl.trackClipById(m);
    CHECK(c->trimInMs == 2000 && c->startMs == 7000 && c->endMs() == 65000);
    // ... and can't push the start below 0: pulled back to where 0 lands.
    tl.moveTrackClip(m, t1, 1000);
    tl.setTrackClipTrimIn(m, 0);
    c = tl.trackClipById(m);
    CHECK(c->startMs == 0 && c->trimInMs == 1000);
    tl.setTrackClipTrimOut(m, 999999); // clamped to the source
    CHECK(tl.trackClipById(m)->trimOutMs == 60000);
    tl.setTrackClipTrimOut(m, 0); // min gap
    CHECK(tl.trackClipById(m)->trimOutMs == 1000 + TrackClip::kMinTrimMs);
    tl.setTrackClipTrimOut(m, 11000);

    // Level clamps; fades clamp to the span, fade-out to what fade-in leaves.
    tl.setAudioClipLevel(m, 99.0, true);
    CHECK(tl.trackClipById(m)->gainDb == audiolimits::kMaxGainDb && tl.trackClipById(m)->muted);
    tl.setTrackClipFades(m, 8000, 5000);
    CHECK(tl.trackClipById(m)->fadeInMs == 8000 && tl.trackClipById(m)->fadeOutMs == 2000);
    tl.setTrackClipTrimOut(m, 6000); // span 5 s: fades shrink with it
    CHECK(tl.trackClipById(m)->fadeInMs == 5000 && tl.trackClipById(m)->fadeOutMs == 0);

    // Move across tracks; clips stay sorted by start.
    const int m2 = tl.addAudioClip(t2, "b.wav", 3000, 9000);
    tl.moveTrackClip(m, t2, 20000);
    CHECK(tl.trackAt(0).clips.isEmpty() && tl.trackAt(1).clips.size() == 2);
    CHECK(tl.trackAt(1).clips[0].id == m2 && tl.trackAt(1).clips[1].id == m);
    tl.moveTrackClip(m, t2, -50);
    CHECK(tl.trackClipById(m)->startMs == 0 && tl.trackAt(1).clips[0].id == m);

    // Rename / reorder.
    CHECK(tl.setTrackName(t1, "  Music  ") && tl.trackById(t1)->name == "Music");
    CHECK(!tl.setTrackName(t1, "   ") && tl.trackById(t1)->name == "Music");
    CHECK(!tl.setTrackName(t1, "Music"));
    CHECK(tl.moveTrack(t2, 0) && tl.trackAt(0).id == t2 && tl.trackAt(1).id == t1);
    CHECK(!tl.moveTrack(t2, -5)); // clamped to 0: already there
    CHECK(tl.moveTrack(t2, 99) && tl.trackAt(1).id == t2);
    CHECK(!tl.moveTrack(999, 0));
    tl.moveTrack(t1, 0);
    tl.setTrackName(t1, "Audio 1");

    tl.setTrackAudio(t2, -99.0, true);
    CHECK(tl.trackById(t2)->gainDb == audiolimits::kMinGainDb && tl.trackById(t2)->muted);

    const int before = changes;
    tl.setAudioClipLevel(m, audiolimits::kMaxGainDb, true); // no-op: no signal
    CHECK(changes == before);

    // Split: the left keeps the fade-in, the right gets the rest and the
    // fade-out, both on the same track, sorted.
    {
        const int s = tl.addAudioClip(t2, "s.wav", 10000, 30000); // 30..40 s
        tl.setTrackClipFades(s, 1000, 2000);
        tl.setAudioClipLevel(s, -5.0, false);
        CHECK(tl.splitTrackClip(s, 30050) == 0); // too close to the edge
        CHECK(tl.splitTrackClip(s, 45000) == 0); // outside
        const int r = tl.splitTrackClip(s, 34000);
        CHECK(r != 0 && r != s);
        const TrackClip *L = tl.trackClipById(s);
        const TrackClip *R = tl.trackClipById(r);
        CHECK(L && R);
        CHECK(L->startMs == 30000 && L->trimInMs == 0 && L->trimOutMs == 4000);
        CHECK(R->startMs == 34000 && R->trimInMs == 4000 && R->trimOutMs == 10000);
        CHECK(L->fadeInMs == 1000 && L->fadeOutMs == 0);
        CHECK(R->fadeInMs == 0 && R->fadeOutMs == 2000 && R->gainDb == -5.0);
        CHECK(tl.trackById(t2)->clips.last().id == r);
        tl.removeTrackClip(s);
        tl.removeTrackClip(r);
    }
    // At speed 0.5 the source point is half the timeline offset.
    {
        TrackClip slow;
        slow.sourcePath = "v.insv";
        slow.sourceDurationMs = 10000;
        slow.startMs = 1000;
        slow.trimOutMs = 4000; // 8 s on the timeline
        slow.speed = 0.5;
        const int s = tl.insertTrackClip(t2, slow);
        const int r = tl.splitTrackClip(s, 5000);
        CHECK(tl.trackClipById(s)->trimOutMs == 2000 && tl.trackClipById(r)->trimInMs == 2000);
        CHECK(tl.trackClipById(r)->startMs == 5000 && tl.trackClipById(r)->endMs() == 9000);
        tl.removeTrackClip(s);
        tl.removeTrackClip(r);
    }

    CHECK(tl.removeTrackClip(m2) && !tl.trackClipById(m2));
    CHECK(tl.removeTrack(t1) && tl.trackCount() == 1);
    tl.clear();
    CHECK(tl.trackCount() == 0 && tl.count() == 0);
}

// A detached mic linked to its video clip, the way
// MainWindow::detachOtherMic makes it.
int addLinkedMic(Timeline &tl, int trackId, int videoId)
{
    const ClipItem *v = tl.clipById(videoId);
    TrackClip a;
    a.sourcePath = v->sourcePath;
    a.sourceDurationMs = v->sourceDurationMs;
    a.stream = 1;
    a.trimInMs = v->trimInMs;
    a.trimOutMs = v->trimOutMs;
    a.speed = v->speed;
    a.linkedClipId = videoId;
    return tl.insertTrackClip(trackId, a);
}

void testLinkedAudio()
{
    Timeline tl;
    const int a = tl.appendClip("a.insv", 10000, 30.0); // 0..10 s
    const int b = tl.appendClip("b.insv", 20000, 30.0); // 10..30 s
    tl.setClipTrimIn(b, 5000);                           // b: 10..25 s, source 5..20 s
    const int t = tl.addTrack(Track::Kind::Audio, "Mics");
    const int m = addLinkedMic(tl, t, b);
    auto mic = [&] { return tl.trackClipById(m); };
    CHECK(mic()->startMs == 10000 && mic()->trimInMs == 5000 && mic()->endMs() == 25000);

    // Follows a reorder.
    tl.moveClip(b, 0); // b first: 0..15 s
    CHECK(mic()->startMs == 0 && mic()->endMs() == 15000);
    tl.moveClip(b, 1);
    CHECK(mic()->startMs == 10000);

    // Follows an insert / removal before it.
    const int c = tl.insertClip(0, "c.insv", 3000, 30.0);
    CHECK(mic()->startMs == 13000);
    tl.removeClip(c);
    CHECK(mic()->startMs == 10000);

    // A linked clip can't be moved, in time or off its track.
    const int t2 = tl.addTrack(Track::Kind::Audio);
    CHECK(!tl.canHoldClip(t2, m) && tl.canHoldClip(t, m));
    tl.moveTrackClip(m, t2, 11000);
    tl.moveTrackClip(m, t, 11000);
    CHECK(mic()->startMs == 10000 && mic()->syncOffsetMs == 0);
    CHECK(tl.trackById(t2)->clips.isEmpty());
    tl.removeTrack(t2);
    // Re-syncing: unlink, move, link again -- the offset comes from the move
    // and survives edits. Linking puts it back on its stream track (it was
    // put on a free one here).
    tl.linkAudioClip(m, 0);
    tl.moveTrackClip(m, t, 10040);
    tl.linkAudioClip(m, b);
    CHECK(mic()->syncOffsetMs == 40 && mic()->startMs == 10040);
    const int cam = tl.streamTrackId(Track::Role::CameraMic);
    CHECK(cam && tl.trackById(cam)->clips.size() == 1 && tl.trackById(t)->clips.isEmpty());
    tl.moveClip(b, 0);
    CHECK(mic()->startMs == 40);
    tl.moveClip(b, 1);

    // Trims: aligned edges follow the video's, both ways.
    tl.setClipTrimIn(b, 7000);
    CHECK(mic()->trimInMs == 7000 && mic()->startMs == 10040);
    tl.setClipTrimOut(b, 15000);
    CHECK(mic()->trimOutMs == 15000 && mic()->endMs() == 18040);
    tl.clearClipTrim(b);
    CHECK(mic()->trimInMs == 0 && mic()->trimOutMs == 20000);
    tl.setClipTrimIn(b, 5000);
    CHECK(mic()->trimInMs == 5000);
    // An edge the user trimmed on the audio itself doesn't follow -- it's only
    // kept inside the video's range.
    tl.setTrackClipTrimOut(m, 12000);
    tl.setClipTrimOut(b, 18000);
    CHECK(mic()->trimOutMs == 12000);
    tl.setClipTrimOut(b, 10000);
    CHECK(mic()->trimOutMs == 10000);
    tl.clearClipTrim(b);
    tl.setClipTrimIn(b, 5000);
    tl.setTrackClipTrimOut(m, 20000);
    CHECK(mic()->trimOutMs == 20000);

    // Speed: the audio takes it, the span and later positions follow.
    tl.setClipSpeed(b, 0.5); // b spans 30 s
    CHECK(mic()->speed == 0.5 && mic()->timelineDurationMs() == 30000);
    CHECK(mic()->startMs == 10040);
    tl.setClipSpeed(a, 2.0); // a shrinks to 5 s: b (and the mic) move up
    CHECK(mic()->startMs == 5040);
    tl.setClipSpeed(a, 1.0);
    tl.setClipSpeed(b, 1.0);

    // Split the video: the mic splits with it, the right part links to the
    // new clip, the offset carried.
    const int b2 = tl.splitClip(b, 12000);
    CHECK(b2 != 0);
    CHECK(mic()->trimOutMs == 12000 && mic()->linkedClipId == b);
    const TrackClip *right = nullptr;
    for (const TrackClip &x : tl.trackById(cam)->clips)
        if (x.linkedClipId == b2)
            right = &x;
    CHECK(right && right->trimInMs == 12000 && right->trimOutMs == 20000);
    CHECK(right && right->startMs == 17040 && right->syncOffsetMs == 40);
    const int rightId = right ? right->id : 0;
    tl.moveClip(b2, 0); // the right part follows its own clip
    CHECK(tl.trackClipById(rightId)->startMs == 40);
    tl.moveClip(b2, 2);

    // Trimming the video past a part the user trimmed on its own removes that
    // part (an aligned edge would just follow).
    tl.setTrackClipTrimIn(rightId, 15000);
    tl.setClipTrimOut(b2, 13000);
    CHECK(!tl.trackClipById(rightId));
    tl.clearClipTrim(b2);
    tl.setClipTrimIn(b2, 12000);
    const int rightId2 = addLinkedMic(tl, cam, b2);
    tl.removeClip(b2); // removing the video removes its mic
    CHECK(!tl.trackClipById(rightId2));

    // Unlink: stops following. Link again: nothing moves, the offset is kept.
    CHECK(tl.linkAudioClip(m, 0));
    CHECK(mic()->linkedClipId == 0);
    tl.moveClip(b, 0);
    CHECK(mic()->startMs == 10040); // stayed put
    CHECK(tl.linkAudioClip(m, b));
    CHECK(mic()->startMs == 10040 && mic()->syncOffsetMs == 10040);
    tl.moveClip(b, 1);
    CHECK(mic()->startMs == 20040);
    CHECK(!tl.linkAudioClip(m, 9999)); // no such clip

    // A music clip never moves with the video.
    const int song = tl.addAudioClip(t, "song.mp3", 5000, 60000);
    tl.moveClip(b, 0);
    CHECK(tl.trackClipById(song)->startMs == 60000);
}

void testProjectTracksJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString projDir = tmp.path();
    const QString video = projDir + "/clip.insv";
    const QString music = projDir + "/music/song.mp3";
    QDir().mkpath(projDir + "/music");
    writeFile(video);
    writeFile(music);
    const QString projFile = projDir + "/tracks.orp";

    {
        Project p;
        p.timeline()->appendClip(video, 10000, 30.0);
        const int t = p.timeline()->addTrack(Track::Kind::Audio, "Music");
        p.timeline()->setTrackAudio(t, -3.0, false);
        const int m = p.timeline()->addAudioClip(t, music, 180000, 1500);
        p.timeline()->setTrackClipTrimIn(m, 500); // start moves to 2000
        p.timeline()->setTrackClipTrimOut(m, 9500);
        p.timeline()->setAudioClipLevel(m, -6.0, false);
        p.timeline()->setTrackClipFades(m, 1000, 2500);
        const int t2 = p.timeline()->addTrack(Track::Kind::Audio);
        TrackClip mic; // a detached camera stream
        mic.sourcePath = video;
        mic.sourceDurationMs = 10000;
        mic.stream = 1;
        mic.trimInMs = 1000;
        mic.trimOutMs = 5000;
        mic.speed = 0.5;
        mic.noise = {insv::NoiseMode::Wind, 0.4f}; // "windGeneral"
        p.timeline()->insertTrackClip(t2, mic);
        TrackClip quiet = mic; // later on the same track, "hiss"
        quiet.startMs = 9000;
        quiet.speed = 1.0;
        quiet.noise = {insv::NoiseMode::Hiss, 0.8f};
        p.timeline()->insertTrackClip(t2, quiet);
        TrackClip talk = quiet; // "voice", "voiceAmbience"
        talk.startMs = 14000;
        talk.noise = {insv::NoiseMode::Voice, 1.0f};
        p.timeline()->insertTrackClip(t2, talk);
        talk.startMs = 19000;
        talk.noise = {insv::NoiseMode::VoiceAmbience, 0.5f};
        p.timeline()->insertTrackClip(t2, talk);
        p.timeline()->setTrackAudio(t2, 0.0, true);
        QString err;
        CHECK(p.save(projFile, &err));
    }

    // The music path is stored relative to the project directory.
    {
        QFile f(projFile);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(text.contains("\"music/song.mp3\""));
        CHECK(text.contains("\"version\":10"));
    }

    {
        Project q;
        QString err;
        CHECK(q.load(projFile, &err));
        const Timeline *tl = q.timeline();
        CHECK(tl->count() == 1 && tl->trackCount() == 2);
        const Track &t = tl->trackAt(0);
        CHECK(t.name == "Music" && t.gainDb == -3.0 && !t.muted && t.clips.size() == 1);
        const TrackClip &m = t.clips[0];
        CHECK(m.sourcePath == QDir::cleanPath(music) && !m.mediaMissing);
        CHECK(m.startMs == 2000 && m.trimInMs == 500 && m.trimOutMs == 9500);
        CHECK(m.gainDb == -6.0 && m.fadeInMs == 1000 && m.fadeOutMs == 2500);
        CHECK(m.stream == -1 && m.speed == 1.0);
        CHECK(m.noise.mode == insv::NoiseMode::Off);
        const Track &t2 = tl->trackAt(1);
        CHECK(t2.name == "Audio 1" && t2.muted && t2.clips.size() == 4);
        CHECK(t2.clips[0].stream == 1 && t2.clips[0].speed == 0.5);
        CHECK(t2.clips[0].noise.mode == insv::NoiseMode::Wind && t2.clips[0].noise.strength == 0.4f);
        CHECK(t2.clips[0].timelineDurationMs() == 8000);
        CHECK(t2.clips[1].startMs == 9000);
        CHECK(t2.clips[1].noise.mode == insv::NoiseMode::Hiss && t2.clips[1].noise.strength == 0.8f);
        CHECK(t2.clips[2].noise.mode == insv::NoiseMode::Voice && t2.clips[2].noise.strength == 1.0f);
        CHECK(t2.clips[3].noise.mode == insv::NoiseMode::VoiceAmbience && t2.clips[3].noise.strength == 0.5f);
    }

    // A file without "tracks" loads with no extra tracks but the clip's
    // sound (an old file: its one stream on the camera mic track).
    {
        const QString noTracks = projDir + "/notracks.orp";
        writeFile(noTracks, QByteArrayLiteral(
                                R"({"format":"orbis-reframe-project","version":8,)"
                                R"("timeline":{"clips":[{"source":"clip.insv","sourceDurationMs":10000,)"
                                R"("fps":30.0}]}})"));
        Project q;
        QString err;
        CHECK(q.load(noTracks, &err));
        CHECK(q.timeline()->count() == 1 && q.timeline()->trackCount() == 1);
        CHECK(q.timeline()->streamTrackId(Track::Role::CameraMic) != 0);
    }

    // A missing music file is flagged, not fatal.
    QFile::remove(music);
    {
        Project q;
        CHECK(q.load(projFile));
        CHECK(q.timeline()->trackAt(0).clips[0].mediaMissing);
    }

    // A link survives save / load and undo (ids change; indices don't).
    {
        const QString linked = projDir + "/linked.orp";
        Project p;
        p.timeline()->appendClip(video, 10000, 30.0);
        const int v2 = p.timeline()->appendClip(video, 10000, 30.0);
        const int t = p.timeline()->addTrack(Track::Kind::Audio, "Mics");
        const int m = addLinkedMic(*p.timeline(), t, v2);
        p.timeline()->linkAudioClip(m, 0); // re-sync by 25 ms
        p.timeline()->moveTrackClip(m, t, 10025);
        p.timeline()->linkAudioClip(m, v2);
        CHECK(p.save(linked));
        Project q;
        CHECK(q.load(linked));
        const TrackClip &a = q.timeline()->trackAt(0).clips[0];
        CHECK(a.linkedClipId == q.timeline()->clipAt(1).id && a.syncOffsetMs == 25);
        CHECK(a.startMs == 10025);
        q.timeline()->moveClip(q.timeline()->clipAt(1).id, 0);
        CHECK(q.timeline()->trackAt(0).clips[0].startMs == 25);
        q.undo();
        const TrackClip &u = q.timeline()->trackAt(0).clips[0];
        CHECK(u.linkedClipId == q.timeline()->clipAt(1).id && u.startMs == 10025);
    }

    // Track edits are undoable.
    {
        Project p;
        p.timeline()->appendClip(video, 10000, 30.0);
        p.undo(); // settle
        p.redo();
        const int t = p.timeline()->addTrack(Track::Kind::Audio);
        p.timeline()->addAudioClip(t, music, 5000, 0);
        CHECK(p.canUndo());
        p.undo();
        CHECK(p.timeline()->trackCount() == 0 && p.timeline()->count() == 1);
        p.redo();
        CHECK(p.timeline()->trackCount() == 1 && p.timeline()->trackAt(0).clips.size() == 1);
    }
}


// Video (overlay) tracks, the inset look and its keyframes, and the
// shared placement math.
void testVideoTracks()
{
    Timeline tl;
    tl.appendClip("/v/a.insv", 60000, 30.0);
    const int a1 = tl.addTrack(Track::Kind::Audio);
    const int v1 = tl.addTrack(Track::Kind::Video);
    const int v2 = tl.addTrack(Track::Kind::Video);
    // The Audio group goes ahead of the Overlay group; each in the order added.
    CHECK(tl.trackAt(0).id == a1 && tl.trackAt(1).id == v1 && tl.trackAt(2).id == v2);
    CHECK(tl.trackAt(0).group() == Track::Group::Audio && tl.trackAt(2).group() == Track::Group::Overlay);
    CHECK(tl.trackAt(1).name == "Overlay 1" && tl.trackAt(2).name == "Overlay 2");
    { // a new track's name is the lowest number free, not a count
        Timeline n;
        n.appendClip("/v/a.insv", 60000, 30.0);
        const int o1 = n.addTrack(Track::Kind::Video);
        n.addTrack(Track::Kind::Video);
        n.setTrackName(o1, "Phone");
        CHECK(n.trackById(n.addTrack(Track::Kind::Video))->name == "Overlay 1");
        CHECK(n.trackById(n.addTrack(Track::Kind::Video))->name == "Overlay 3");
    }
    // Reordering stays inside the group.
    CHECK(!tl.moveTrack(a1, 2) && tl.trackAt(0).id == a1);
    CHECK(tl.moveTrack(v2, 0) && tl.trackAt(1).id == v2);
    CHECK(!tl.moveTrack(v2, 5) || tl.trackAt(2).id == v2);
    tl.moveTrack(v1, 1);
    CHECK(tl.trackAt(1).id == v1 && tl.trackAt(2).id == v2);

    // Clips only go on a track of their kind.
    CHECK(tl.addAudioClip(v1, "/m/song.mp3", 5000, 0) == 0);
    CHECK(tl.addVideoClip(a1, "/v/pip.mp4", 5000, 0, 1920, 1080) == 0);
    const int c = tl.addVideoClip(v1, "/v/pip.mp4", 8000, 2000, 1920, 1080);
    CHECK(c > 0);
    const TrackClip *pc = tl.trackClipById(c);
    CHECK(pc && pc->startMs == 2000 && pc->endMs() == 10000 && pc->sourceWidth == 1920);
    CHECK(std::abs(pc->aspect() - 16.0 / 9.0) < 1e-9);
    const int m = tl.addAudioClip(a1, "/m/song.mp3", 5000, 0);
    tl.moveTrackClip(c, a1, 3000); // wrong kind: nothing moves
    CHECK(tl.trackClipById(c)->startMs == 2000 && tl.trackById(v1)->clips.size() == 1);
    tl.moveTrackClip(c, v2, 3000);
    CHECK(tl.trackById(v2)->clips.size() == 1 && tl.trackClipById(c)->startMs == 3000);
    CHECK(!tl.linkAudioClip(c, tl.clipAt(0).id)); // no audio link on an overlay
    tl.moveTrackClip(m, a1, 100);
    CHECK(tl.trackClipById(m)->startMs == 100);

    // The static look and frame are clamped.
    inset::Look look;
    look.centerX = 0.25f;
    look.centerY = 0.3f;
    look.width = 5.0f; // > 1
    look.opacity = 0.5f;
    inset::Frame frame;
    frame.cornerRadius = 0.9f; // > 0.5
    frame.borderWidth = 0.01f;
    frame.borderRgb = 0x12ff3456; // alpha bits dropped
    tl.setTrackClipInset(c, look, frame);
    pc = tl.trackClipById(c);
    CHECK(pc->look.width == inset::Look::kMaxWidth && pc->look.centerX == 0.25f);
    CHECK(pc->frame.cornerRadius == 0.5f && pc->frame.borderRgb == 0xff3456);

    // Keyframes: held before / after, linear between; removing the last one
    // keeps its look as the static one.
    inset::Look k0 = pc->look, k1 = pc->look;
    k0.centerX = 0.2f;
    k1.centerX = 0.6f;
    k1.opacity = 1.0f;
    tl.upsertTrackClipInsetKey(c, 1000, k0);
    tl.upsertTrackClipInsetKey(c, 3000, k1);
    pc = tl.trackClipById(c);
    CHECK(pc->insetKeys.count() == 2);
    CHECK(pc->lookAt(0).centerX == 0.2f && pc->lookAt(5000).centerX == 0.6f);
    CHECK(std::abs(pc->lookAt(2000).centerX - 0.4f) < 1e-6f);
    CHECK(std::abs(pc->lookAt(2000).opacity - 0.75f) < 1e-6f);
    tl.upsertTrackClipInsetKey(c, 3010, k0); // snaps onto the 3000 key
    CHECK(tl.trackClipById(c)->insetKeys.count() == 2);
    // A split copies the keys to both halves.
    const int right = tl.splitTrackClip(c, 5000); // source 2000
    CHECK(right > 0 && tl.trackClipById(right)->insetKeys.count() == 2);
    tl.removeTrackClip(right);
    tl.removeTrackClipInsetKeyNear(c, 1100);
    CHECK(tl.trackClipById(c)->insetKeys.count() == 1);
    tl.removeTrackClipInsetKeyNear(c, 2950);
    pc = tl.trackClipById(c);
    CHECK(pc->insetKeys.isEmpty() && pc->look.centerX == 0.2f);

    // Placement: 1920x1080 output, a 16:9 inset 30% wide centred at (0.75, 0.5).
    inset::Look l;
    l.centerX = 0.75f;
    l.centerY = 0.5f;
    l.width = 0.3f;
    inset::Frame f;
    f.cornerRadius = 0.1f;
    f.borderWidth = 0.005f;
    const inset::Rect r = inset::place(l, f, 16.0 / 9.0, 1920, 1080);
    CHECK(std::abs(r.w - 576.0) < 1e-3 && std::abs(r.h - 324.0) < 1e-3);
    CHECK(std::abs(r.x - (1440.0 - 288.0)) < 1e-3 && std::abs(r.y - (540.0 - 162.0)) < 1e-3);
    CHECK(std::abs(r.radius - 32.4) < 1e-3 && std::abs(r.border - 5.4) < 1e-3);
    // Inside: covered, no border; outside: nothing; on the edge: the border.
    inset::Coverage in = inset::shade(1440.5, 540.5, r);
    CHECK(in.alpha == 1.0f && in.border == 0.0f);
    CHECK(inset::shade(100.5, 100.5, r).alpha == 0.0f);
    inset::Coverage edge = inset::shade(r.x + 2.0, 540.0, r);
    CHECK(edge.alpha == 1.0f && edge.border == 1.0f);
    // The corner is cut off by the radius.
    CHECK(inset::shade(r.x + 1.0, r.y + 1.0, r).alpha == 0.0f);
    // Fades.
    CHECK(inset::fadeGain(500, 8000, 1000, 0) == 0.5f);
    CHECK(inset::fadeGain(7750, 8000, 0, 1000) == 0.25f);
    CHECK(inset::fadeGain(4000, 8000, 1000, 1000) == 1.0f);
}

// v8: video tracks round-trip through the .orp.
// An overlay's sound on an audio track, the way
// MainWindow::putOverlayAudioOnTrack makes it.
int addOverlayAudio(Timeline &tl, int trackId, int overlayId)
{
    const TrackClip *o = tl.trackClipById(overlayId);
    TrackClip a;
    a.sourcePath = o->sourcePath;
    a.sourceDurationMs = o->sourceDurationMs;
    a.startMs = o->startMs;
    a.trimInMs = o->trimInMs;
    a.trimOutMs = o->trimOutMs;
    a.linkedClipId = overlayId;
    return tl.insertTrackClip(trackId, a);
}

void testOverlayAudio()
{
    Timeline tl;
    tl.appendClip("/v/a.insv", 60000, 30.0);
    const int v = tl.addTrack(Track::Kind::Video);
    const int v2 = tl.addTrack(Track::Kind::Video);
    const int t = tl.addTrack(Track::Kind::Audio);
    const int o = tl.addVideoClip(v, "/v/pip.mp4", 10000, 2000, 1920, 1080); // 2..12 s
    const int s = addOverlayAudio(tl, t, o);
    auto snd = [&] { return tl.trackClipById(s); };
    const auto trackOf = [&](int clipId) {
        int id = 0;
        tl.trackClipById(clipId, &id);
        return id;
    };
    CHECK(snd() && snd()->linkedClipId == o && snd()->syncOffsetMs == 0);
    CHECK(snd()->startMs == 2000 && snd()->endMs() == 12000);
    // Not on the free track it was put on: on its overlay track's sound
    // track, made right under it.
    const int vs = tl.audioTrackId(v);
    CHECK(vs && trackOf(s) == vs && tl.trackById(t)->clips.isEmpty());
    CHECK(tl.trackIndexOfId(vs) == tl.trackIndexOfId(v) + 1);
    CHECK(tl.trackById(vs)->name == "Overlay 1 audio" && tl.trackById(vs)->ownerTrackId == v);
    tl.setTrackName(v, "Phone"); // the audio track's name follows
    CHECK(tl.trackById(vs)->name == "Phone audio");

    // Follows a move, also onto another overlay track (and its audio track;
    // the one it leaves empty goes).
    tl.moveTrackClip(o, v, 5000);
    CHECK(snd()->startMs == 5000);
    tl.moveTrackClip(o, v2, 4000);
    CHECK(snd()->startMs == 4000 && snd()->linkedClipId == o);
    const int v2s = tl.audioTrackId(v2);
    CHECK(v2s && trackOf(s) == v2s && !tl.trackById(vs) && !tl.audioTrackId(v));
    CHECK(tl.trackIndexOfId(v2s) == tl.trackIndexOfId(v2) + 1);
    // Can't be moved in time on its own, nor off its audio track.
    tl.moveTrackClip(s, t, 9000);
    CHECK(snd()->startMs == 4000 && trackOf(s) == v2s);

    // Follows trims: head (the overlay's start moves with it) and tail.
    tl.setTrackClipTrimIn(o, 1000); // overlay 5..13 s, source 1..10 s
    CHECK(tl.trackClipById(o)->startMs == 5000);
    CHECK(snd()->trimInMs == 1000 && snd()->startMs == 5000);
    tl.setTrackClipTrimOut(o, 8000);
    CHECK(snd()->trimOutMs == 8000 && snd()->endMs() == 12000);

    // Splits with it: the part past the cut follows the right half.
    const int o2 = tl.splitTrackClip(o, 8000); // at source 4 s
    CHECK(o2 > 0);
    CHECK(snd()->trimOutMs == 4000 && snd()->linkedClipId == o);
    int s2 = 0;
    for (const TrackClip &x : tl.trackById(v2s)->clips)
        if (x.linkedClipId == o2)
            s2 = x.id;
    CHECK(s2 > 0 && tl.trackClipById(s2)->trimInMs == 4000);
    CHECK(tl.trackClipById(s2)->startMs == 8000);
    tl.moveTrackClip(o2, v2, 20000);
    CHECK(tl.trackClipById(s2)->startMs == 20000);

    // Linking by hand keeps the position (and puts it back on the sound
    // track); an audio clip is no target.
    CHECK(tl.linkAudioClip(s, 0) && snd()->linkedClipId == 0);
    tl.moveTrackClip(s, t, 5040);
    CHECK(trackOf(s) == t);
    CHECK(tl.linkAudioClip(s, o) && snd()->syncOffsetMs == 40 && snd()->startMs == 5040);
    CHECK(trackOf(s) == v2s);
    tl.moveTrackClip(o, v2, 6000);
    CHECK(snd()->startMs == 6040);
    CHECK(!tl.linkAudioClip(s, s2));

    // Removed with it: the clip, or its whole track.
    tl.removeTrackClip(o);
    CHECK(!tl.trackClipById(s));
    tl.removeTrack(v2);
    CHECK(!tl.trackClipById(s2) && tl.trackById(t)->clips.isEmpty() && !tl.trackById(v2s));
}

// The track groups: the main video's sound, Audio, then each overlay track
// with its audio track; reordering stays within a group, an overlay track
// moving with its audio track.
void testTrackGroups()
{
    Timeline tl;
    tl.appendClip("/v/a.insv", 60000, 30.0);
    const int o1 = tl.addTrack(Track::Kind::Video);
    const int a1 = tl.addTrack(Track::Kind::Audio);
    const int o2 = tl.addTrack(Track::Kind::Video);
    tl.addClipSound(tl.clipAt(0).id, 2); // external mic + camera mic
    const int a2 = tl.addTrack(Track::Kind::Audio);
    const int o3 = tl.addTrack(Track::Kind::Video);
    const int cam = tl.streamTrackId(Track::Role::CameraMic);
    const int ext = tl.streamTrackId(Track::Role::ExternalMic);
    const int s1 = tl.addTrack(Track::Kind::Audio, QString(), Track::Role::OverlayAudio, o1);
    const int s3 = tl.addTrack(Track::Kind::Audio, QString(), Track::Role::OverlayAudio, o3);
    CHECK(tl.addTrack(Track::Kind::Audio, QString(), Track::Role::OverlayAudio, a1) == 0);
    const auto order = [&] {
        QList<int> ids;
        for (int i = 0; i < tl.trackCount(); ++i)
            ids.append(tl.trackAt(i).id);
        return ids;
    };
    CHECK((order() == QList<int>{cam, ext, a1, a2, o1, s1, o2, o3, s3}));

    // Within the group only.
    CHECK(!tl.moveTrack(ext, 5) && !tl.moveTrack(a1, 0)); // clamped: already there
    CHECK(tl.moveTrack(a1, 8) && (order() == QList<int>{cam, ext, a2, a1, o1, s1, o2, o3, s3}));
    CHECK(tl.moveTrack(ext, 0) && (order() == QList<int>{ext, cam, a2, a1, o1, s1, o2, o3, s3}));
    // A audio track only moves with its overlay track, which takes it along.
    CHECK(!tl.moveTrack(s1, 6));
    CHECK(tl.moveTrack(o1, 6) && (order() == QList<int>{ext, cam, a2, a1, o2, o1, s1, o3, s3}));
    CHECK(tl.moveTrack(o1, 8) && (order() == QList<int>{ext, cam, a2, a1, o2, o3, s3, o1, s1}));
    CHECK(!tl.moveTrack(o1, 8)); // its own audio track: already there
    CHECK(tl.moveTrack(o3, 4) && (order() == QList<int>{ext, cam, a2, a1, o3, s3, o2, o1, s1}));
    CHECK(tl.moveTrack(o2, 0) && (order() == QList<int>{ext, cam, a2, a1, o2, o3, s3, o1, s1}));
    // New tracks at the end of their group.
    const int a3 = tl.addTrack(Track::Kind::Audio);
    CHECK(tl.trackIndexOfId(a3) == 4);
    // Removing an overlay track takes its audio track.
    CHECK(tl.removeTrack(o3) && !tl.trackById(s3));
}

// Unlinked audio stays where it is; the video and overlay groups' audio
// tracks hold only their own source's sound.
void testOwnSoundTracks()
{
    Timeline tl;
    tl.setAudioStreamCounter([](const QString &path) { return path.endsWith(".insv") ? 2 : 1; });
    const int a = tl.appendClip("/v/a.insv", 10000, 30.0, 0, 0, 2); // 0..10 s
    const int b = tl.appendClip("/v/b.insv", 10000, 30.0, 0, 0, 2); // 10..20 s
    const int cam = tl.streamTrackId(Track::Role::CameraMic);
    const int ext = tl.streamTrackId(Track::Role::ExternalMic);
    const int music = tl.addTrack(Track::Kind::Audio);
    const int ov = tl.addTrack(Track::Kind::Video);
    const int pip = tl.addVideoClip(ov, "/v/pip.mp4", 5000, 1000, 1920, 1080);
    const int song = tl.addAudioClip(music, "/m/song.mp3", 30000, 0);
    const auto trackOf = [&](int clipId) {
        int id = 0;
        tl.trackClipById(clipId, &id);
        return id;
    };
    int camA = 0, extA = 0;
    for (const TrackClip &x : tl.trackById(cam)->clips)
        if (x.linkedClipId == a)
            camA = x.id;
    for (const TrackClip &x : tl.trackById(ext)->clips)
        if (x.linkedClipId == a)
            extA = x.id;
    CHECK(camA && extA);

    // Music goes on no video's sound track; a mic only on its own.
    CHECK(!tl.canHoldClip(cam, song) && !tl.canHoldClip(ext, song));
    tl.moveTrackClip(song, cam, 500);
    CHECK(trackOf(song) == music && tl.trackClipById(song)->startMs == 0);
    CHECK(!tl.canHoldClip(music, camA) && !tl.canHoldClip(ext, camA));

    // Unlinking moves nothing; the clip may then be nudged on its track, go
    // to the Audio group and come back, but not onto the other mic's track.
    CHECK(tl.linkAudioClip(camA, 0));
    CHECK(trackOf(camA) == cam && tl.trackClipById(camA)->startMs == 0);
    tl.moveTrackClip(camA, cam, 40);
    CHECK(tl.trackClipById(camA)->startMs == 40);
    tl.moveTrackClip(camA, ext, 40);
    CHECK(trackOf(camA) == cam);
    tl.moveTrackClip(camA, music, 40);
    CHECK(trackOf(camA) == music);
    CHECK(tl.canHoldClip(cam, camA) && !tl.canHoldClip(ext, camA));
    // Linked again in the Audio group: back on its stream track, time kept.
    CHECK(tl.linkAudioClip(camA, a));
    CHECK(trackOf(camA) == cam && tl.trackClipById(camA)->startMs == 40);

    // Removing a video clip takes its linked audio only.
    int camB = 0;
    for (const TrackClip &x : tl.trackById(cam)->clips)
        if (x.linkedClipId == b)
            camB = x.id;
    CHECK(tl.linkAudioClip(extA, 0));
    tl.removeClip(a);
    CHECK(!tl.trackClipById(camA) && tl.trackClipById(extA) && trackOf(extA) == ext);
    CHECK(tl.trackClipById(camB)); // b's own sound is untouched

    // An overlay's audio track: only that overlay's sound.
    const int snd = addOverlayAudio(tl, music, pip);
    const int ovs = tl.audioTrackId(ov);
    CHECK(ovs && trackOf(snd) == ovs);
    CHECK(!tl.canHoldClip(ovs, song) && !tl.canHoldClip(ovs, extA));
    CHECK(!tl.canHoldClip(music, snd) && !tl.canHoldClip(cam, snd));
    CHECK(tl.linkAudioClip(snd, 0) && trackOf(snd) == ovs);
    CHECK(tl.canHoldClip(music, snd) && !tl.canHoldClip(cam, snd));
    tl.moveTrackClip(snd, music, 1000); // the empty audio track goes
    CHECK(trackOf(snd) == music && !tl.audioTrackId(ov));
}

void testProjectVideoTracksJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString dir = tmp.path();
    const QString video = dir + "/clip.insv";
    const QString pip = dir + "/pip/cam.mp4";
    QDir().mkpath(dir + "/pip");
    writeFile(video);
    writeFile(pip);
    const QString projFile = dir + "/video.orp";
    {
        Project p;
        p.timeline()->appendClip(video, 10000, 30.0);
        p.timeline()->addTrack(Track::Kind::Audio, "Music");
        const int t = p.timeline()->addTrack(Track::Kind::Video, "Cam");
        const int c = p.timeline()->addVideoClip(t, pip, 6000, 1000, 1280, 720);
        p.timeline()->setTrackClipTrimIn(c, 500);
        p.timeline()->setTrackClipFades(c, 300, 400);
        inset::Look l;
        l.centerX = 0.3f;
        l.width = 0.4f;
        inset::Frame f;
        f.cornerRadius = 0.2f;
        f.borderWidth = 0.004f;
        f.borderRgb = 0x00ff80;
        p.timeline()->setTrackClipInset(c, l, f);
        l.opacity = 0.25f;
        p.timeline()->upsertTrackClipInsetKey(c, 2000, l);
        p.timeline()->setTrackAudio(t, 0.0, true); // hidden
        CHECK(p.save(projFile));
    }
    {
        QFile f(projFile);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(text.contains("\"kind\":\"video\"") && text.contains("\"pip/cam.mp4\""));
        CHECK(text.contains("\"borderColor\":\"#00ff80\""));
    }
    Project q;
    QString err;
    CHECK(q.load(projFile, &err));
    const Timeline *tl = q.timeline();
    CHECK(tl->trackCount() == 2);
    const Track &t = tl->trackAt(1);
    CHECK(t.kind == Track::Kind::Video && t.name == "Cam" && t.muted && t.clips.size() == 1);
    const TrackClip &c = t.clips[0];
    CHECK(c.sourcePath == QDir::cleanPath(pip) && c.sourceWidth == 1280 && c.sourceHeight == 720);
    CHECK(c.startMs == 1500 && c.trimInMs == 500 && c.fadeInMs == 300 && c.fadeOutMs == 400);
    CHECK(c.look.centerX == 0.3f && c.look.width == 0.4f);
    CHECK(c.frame.cornerRadius == 0.2f && c.frame.borderWidth == 0.004f);
    CHECK(c.frame.borderRgb == 0x00ff80);
    CHECK(c.insetKeys.count() == 1 && c.insetKeys.keys()[0].timeMs == 2000);
    CHECK(c.insetKeys.keys()[0].look.opacity == 0.25f);
    CHECK(tl->trackAt(0).kind == Track::Kind::Audio && tl->trackAt(0).name == "Music");

    // An overlay's linked sound survives save / load and undo.
    const QString linked = dir + "/sound.orp";
    {
        Project p;
        CHECK(p.load(projFile));
        Timeline *pt = p.timeline();
        const int o = pt->trackAt(1).clips[0].id;
        const int a = pt->trackAt(0).id;
        const int snd = addOverlayAudio(*pt, a, o); // onto Cam's audio track
        pt->linkAudioClip(snd, 0);
        pt->moveTrackClip(snd, pt->audioTrackId(pt->trackAt(1).id),
                          pt->trackClipById(snd)->startMs + 30); // a 30 ms nudge
        pt->linkAudioClip(snd, o);
        pt->setTrackAudio(pt->audioTrackId(pt->trackAt(1).id), -3.0, false);
        CHECK(p.save(linked));
    }
    Project r;
    CHECK(r.load(linked));
    const Timeline *rt = r.timeline();
    // Music, Cam, Cam's sound.
    CHECK(rt->trackCount() == 3 && rt->trackAt(2).role == Track::Role::OverlayAudio);
    CHECK(rt->trackAt(2).ownerTrackId == rt->trackAt(1).id && rt->trackAt(2).name == "Cam audio");
    CHECK(rt->trackAt(2).gainDb == -3.0 && rt->trackAt(0).clips.isEmpty());
    const TrackClip &ov = rt->trackAt(1).clips[0];
    CHECK(rt->trackAt(2).clips.size() == 1);
    const TrackClip &rs = rt->trackAt(2).clips[0];
    CHECK(rs.linkedClipId == ov.id && rs.syncOffsetMs == 30 && rs.startMs == ov.startMs + 30);
    r.timeline()->moveTrackClip(ov.id, rt->trackAt(1).id, 3000);
    CHECK(rt->trackAt(2).clips[0].startMs == 3030);
    r.undo();
    CHECK(rt->trackAt(2).clips[0].linkedClipId == rt->trackAt(1).clips[0].id);
    CHECK(rt->trackAt(2).clips[0].startMs == 1530);

    // From before overlay audio tracks: the sound on a free track moves to
    // its overlay's audio track, and the free track it leaves empty goes.
    {
        QFile f(linked);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonDocument d = QJsonDocument::fromJson(f.readAll());
        QJsonObject doc = d.object(), tlj = doc["timeline"].toObject();
        QJsonArray tracks = tlj["tracks"].toArray();
        QJsonObject music = tracks[0].toObject(), sound = tracks[2].toObject();
        music["clips"] = sound["clips"];
        tracks = QJsonArray{music, tracks[1]};
        tlj["tracks"] = tracks;
        doc["timeline"] = tlj;
        f.close();
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(doc).toJson());
    }
    Project old;
    CHECK(old.load(linked));
    const Timeline *ot = old.timeline();
    CHECK(ot->trackCount() == 2 && ot->trackAt(0).kind == Track::Kind::Video);
    CHECK(ot->trackAt(1).role == Track::Role::OverlayAudio && ot->trackAt(1).clips.size() == 1);
    CHECK(ot->trackAt(1).clips[0].linkedClipId == ot->trackAt(0).clips[0].id);
}

} // namespace

void testKeyboardNavigation()
{
    Timeline tl;
    CHECK(tl.stepFrames(500, 1) == 0); // empty: clamped to [0, 0]
    const int a = tl.appendClip("a.insv", 10000, 30.0); // 33.3 ms frames
    tl.appendClip("b.insv", 6000, 25.0);                // 40 ms frames

    // Frame steps land on a frame's start, rounded up (a paused seek shows
    // the first frame at or after the target).
    CHECK(tl.stepFrames(0, 1) == 34);
    CHECK(tl.stepFrames(34, 1) == 67);
    CHECK(tl.stepFrames(67, -1) == 34);
    CHECK(tl.stepFrames(34, -1) == 0);
    CHECK(tl.stepFrames(0, -1) == 0); // nothing before the start
    CHECK(tl.stepFrames(50, 0) == 50);
    CHECK(tl.stepFrames(50, 1) == 100); // 50 shows frame 2 (67): on to frame 3
    // Across the cut: b's first frame, and back to a's last one.
    CHECK(tl.stepFrames(9990, 1) == 10000);
    CHECK(tl.stepFrames(10000, -1) == 9967);
    CHECK(tl.stepFrames(10000, 1) == 10040); // b's own frame rate
    // At the end.
    CHECK(tl.stepFrames(16000, 1) == 16000);
    CHECK(tl.stepFrames(16000, -1) == 15960);

    // Edit points: start, the cut, the end, an audio clip's start (its end
    // lies past the main timeline).
    const int t = tl.addTrack(Track::Kind::Audio);
    tl.addAudioClip(t, "song.mp3", 60000, 5000);
    CHECK(tl.adjacentEditPoint(0, 1) == 5000);
    CHECK(tl.adjacentEditPoint(5000, 1) == 10000);
    CHECK(tl.adjacentEditPoint(10000, 1) == 16000);
    CHECK(tl.adjacentEditPoint(16000, 1) == 16000); // nothing further
    CHECK(tl.adjacentEditPoint(10000, -1) == 5000);
    CHECK(tl.adjacentEditPoint(7000, -1) == 5000);
    CHECK(tl.adjacentEditPoint(0, -1) == 0);

    // Slow motion: a source frame spans more timeline.
    tl.setClipSpeed(a, 0.5);
    CHECK(tl.stepFrames(0, 1) == 68);
    CHECK(tl.stepFrames(68, 1) == 134);
    CHECK(tl.adjacentEditPoint(5000, 1) == 20000); // the cut moved
}

void testKeyNavigation()
{
    Timeline tl;
    const int a = tl.appendClip("a.insv", 10000, 30.0);
    const int b = tl.appendClip("b.insv", 6000, 25.0);
    CHECK(tl.adjacentKey(0, 1) == 0); // no keys
    tl.upsertClipReframeKey(a, 1000, {});
    tl.upsertClipReframeKey(a, 8000, {});
    tl.upsertClipReframeKey(b, 2000, {}); // timeline 12000
    // Across clips, in timeline time.
    CHECK(tl.adjacentKey(0, 1) == 1000);
    CHECK(tl.adjacentKey(1000, 1) == 8000);
    CHECK(tl.adjacentKey(8000, 1) == 12000);
    CHECK(tl.adjacentKey(12000, 1) == 12000); // nothing further
    CHECK(tl.adjacentKey(12000, -1) == 8000);
    CHECK(tl.adjacentKey(1000, -1) == 1000);
    // A playhead a rounded ms off a key counts as on it.
    CHECK(tl.adjacentKey(7998, 1) == 12000);
    CHECK(tl.adjacentKey(1002, -1) == 1002);
    // Trimmed out of view: skipped. Trimming a's head moves b's key too.
    tl.setClipTrimIn(a, 2000);
    CHECK(tl.adjacentKey(0, 1) == 6000);
    CHECK(tl.adjacentKey(6000, 1) == 10000);
    // Slow motion stretches the key's place.
    tl.setClipSpeed(a, 0.5);
    CHECK(tl.adjacentKey(0, 1) == 12000);

    // An overlay clip's keys, when asked for: source -> timeline.
    const int t = tl.addTrack(Track::Kind::Video);
    const int o = tl.addVideoClip(t, "phone.mp4", 20000, 3000, 1080, 1920);
    tl.upsertTrackClipInsetKey(o, 500, {});
    tl.upsertTrackClipInsetKey(o, 4000, {});
    CHECK(tl.adjacentKey(0, 1, o) == 3500);
    CHECK(tl.adjacentKey(3500, 1, o) == 7000);
    CHECK(tl.adjacentKey(9000, -1, o) == 7000);
    CHECK(tl.adjacentKey(0, 1) == 12000); // the reframe keys, as before
}

void testPerClipStabilization()
{
    const insv::StabConfig lean = insv::StabConfig::preset(insv::StabMode::LeanFollow);
    const insv::StabConfig level = insv::StabConfig::preset(insv::StabMode::HorizonLock);

    // A new clip takes the setting of the clip before it (after it when it
    // goes first); the first one on an empty timeline starts Off.
    {
        Timeline tl;
        int changed = 0;
        QObject::connect(&tl, &Timeline::clipStabilizationChanged, [&](int, insv::StabConfig) {
            ++changed;
        });
        const int a = tl.appendClip("a.insv", 10000, 30.0);
        CHECK(tl.clipById(a)->stabilization == insv::StabConfig{});
        tl.setClipStabilization(a, lean);
        CHECK(changed == 1);
        tl.setClipStabilization(a, lean); // unchanged: no signal
        CHECK(changed == 1);
        const int b = tl.appendClip("b.insv", 10000, 30.0);
        CHECK(tl.clipById(b)->stabilization == lean);
        tl.setClipStabilization(b, level);
        const int c = tl.insertClip(1, "c.insv", 10000, 30.0); // between a and b
        CHECK(tl.clipById(c)->stabilization == lean);
        const int d = tl.insertClip(0, "d.insv", 10000, 30.0); // first: takes a's
        CHECK(tl.clipById(d)->stabilization == lean);
        CHECK(tl.clipById(b)->stabilization == level); // untouched
        // A split keeps the setting on both halves.
        const int e = tl.splitClip(b, 5000);
        CHECK(e != 0 && tl.clipById(e)->stabilization == level);
    }

    // Undo covers it.
    {
        Project p;
        p.timeline()->appendClip("a.insv", 10000, 30.0);
        p.undo(); // settle
        p.redo();
        const int id = p.timeline()->clipAt(0).id; // undo()/redo() rebuild -> fresh id
        p.timeline()->setClipStabilization(id, level);
        CHECK(p.modified());
        p.undo();
        CHECK(p.timeline()->clipAt(0).stabilization == insv::StabConfig{});
        p.redo();
        CHECK(p.timeline()->clipAt(0).stabilization == level);
    }

    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    // Saved per clip, with no project-wide setting left in the file.
    {
        const QString file = tmp.path() + "/perclip.orp";
        Project p;
        const int a = p.timeline()->appendClip("a.insv", 10000, 30.0);
        p.timeline()->appendClip("b.insv", 10000, 30.0);
        p.timeline()->setClipStabilization(a, level);
        QString err;
        CHECK(p.save(file, &err));
        QFile f(file);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(text.contains("\"version\":10"));
        CHECK(text.count("\"stabilization\"") == 2); // one per clip

        Project q;
        CHECK(q.load(file, &err));
        CHECK(q.timeline()->clipAt(0).stabilization == level);
        CHECK(q.timeline()->clipAt(1).stabilization == insv::StabConfig{});
    }

    // A version 8 (0.1.0) project: its project-wide setting goes onto every
    // clip, and it opens unmodified.
    {
        const QString file = tmp.path() + "/v8.orp";
        writeFile(file, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":8,)"
                            R"("stabilization":{"mode":"horizonLock","horizonLockAmount":0.5},)"
                            R"("timeline":{"clips":[)"
                            R"({"source":"a.insv","sourceDurationMs":1000,"fps":30},)"
                            R"({"source":"b.insv","sourceDurationMs":1000,"fps":30,)"
                            R"("stabilization":{"mode":"leanFollow"}}]}})"));
        Project p;
        QString err;
        CHECK(p.load(file, &err));
        CHECK(err.isEmpty());
        CHECK(!p.modified());
        CHECK(p.timeline()->count() == 2);
        for (int i = 0; i < p.timeline()->count(); ++i) {
            const insv::StabConfig &st = p.timeline()->clipAt(i).stabilization;
            CHECK(st.mode == insv::StabMode::HorizonLock); // a per-clip value is ignored in v8
            CHECK(st.horizonLockAmount == 0.5f);
        }
    }
}

void testProjectFolder()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString parent = tmp.path();

    CHECK(!projectfolder::nameProblem(QString()).isEmpty());
    CHECK(!projectfolder::nameProblem("  ").isEmpty());
    CHECK(!projectfolder::nameProblem(" trip").isEmpty());
    CHECK(!projectfolder::nameProblem("a/b").isEmpty());
    CHECK(!projectfolder::nameProblem(".hidden").isEmpty());
    CHECK(!projectfolder::nameProblem(QString(201, QLatin1Char('x'))).isEmpty());
    CHECK(projectfolder::nameProblem("Summer trip 2026").isEmpty());

    QString error;
    const QString file = projectfolder::create(parent, "Summer trip", &error);
    CHECK(error.isEmpty());
    CHECK(file == parent + "/Summer trip/Summer trip.orp");
    CHECK(!QFileInfo::exists(file)); // created by the first save, not here
    for (const char *sub : {"cache", "exports", "screenshots"})
        CHECK(QFileInfo(parent + "/Summer trip/" + sub).isDir());
    CHECK(projectfolder::isProjectFolder(file));
    CHECK(projectfolder::cacheDir(file) == parent + "/Summer trip/cache");
    CHECK(projectfolder::exportsDir(file) == parent + "/Summer trip/exports");
    CHECK(projectfolder::screenshotsDir(file) == parent + "/Summer trip/screenshots");

    // Taken: refused, nothing changed.
    CHECK(projectfolder::create(parent, "Summer trip", &error).isEmpty());
    CHECK(!error.isEmpty());
    CHECK(projectfolder::create(parent, "a/b", &error).isEmpty());

    // A loose project file has no folders of its own.
    const QString loose = parent + "/old.orp";
    CHECK(!projectfolder::isProjectFolder(loose));
    CHECK(projectfolder::cacheDir(loose).isEmpty());
    CHECK(projectfolder::exportsDir(QString()).isEmpty());

    // ensure() remakes a folder deleted by hand.
    QDir(parent + "/Summer trip/exports").removeRecursively();
    CHECK(projectfolder::ensure(projectfolder::exportsDir(file)) == parent + "/Summer trip/exports");
    CHECK(QFileInfo(parent + "/Summer trip/exports").isDir());
    CHECK(projectfolder::ensure(QString()).isEmpty());
}

void testProjectLibrary()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString listFile = tmp.path() + "/data/projects.json";
    const QString folder = tmp.path() + "/Projects";

    // Two project folders in the projects folder, one loose file elsewhere,
    // and a folder that isn't a project.
    const QString a = projectfolder::create(folder, "Alpha");
    const QString b = projectfolder::create(folder, "Beta");
    writeFile(a);
    writeFile(b);
    QDir().mkpath(folder + "/not a project");
    const QString loose = tmp.path() + "/loose.orp";
    writeFile(loose);

    const QDateTime t0 = QDateTime::fromString("2026-10-01T10:00:00.000", Qt::ISODateWithMs);
    {
        ProjectLibrary lib(listFile);
        lib.load(); // no file yet
        CHECK(lib.projectsFolder() == ProjectLibrary::standardProjectsFolder());
        CHECK(lib.setProjectsFolder(folder));
        auto e = lib.entries();
        CHECK(e.size() == 2); // found by the scan
        for (const auto &x : e)
            CHECK(!x.lastOpened.isValid());

        CHECK(lib.noteOpened(loose, t0));
        CHECK(lib.noteOpened(b, t0.addSecs(60)));
        e = lib.entries();
        CHECK(e.size() == 3);
        if (e.size() == 3) {
            // Newest first; found-only projects sort by their file time (now).
            CHECK(e[0].path == a && !e[0].lastOpened.isValid());
            CHECK(e[1].path == b && e[1].lastOpened == t0.addSecs(60));
            CHECK(e[2].path == loose);
        }
    }
    {
        // Persisted: a fresh library reads the same list and folder.
        ProjectLibrary lib(listFile);
        lib.load();
        CHECK(lib.projectsFolder() == folder);
        CHECK(lib.entries().size() == 3);

        // Remove from list: a listed loose file is gone; a found project is
        // hidden (its files stay) until it's opened again.
        CHECK(lib.remove(loose));
        CHECK(lib.remove(a));
        auto e = lib.entries();
        CHECK(e.size() == 1 && e[0].path == b);
        CHECK(QFileInfo::exists(a) && QFileInfo::exists(loose));
        CHECK(lib.noteOpened(a, t0.addSecs(120)));
        e = lib.entries();
        CHECK(e.size() == 2 && e[0].path == a);

        // A listed project whose file is gone stays listed (the card says so).
        QFile::remove(b);
        CHECK(lib.entries().size() == 2);
    }
    {
        ProjectLibrary lib(listFile);
        lib.load();
        CHECK(lib.entries().size() == 2);
        CHECK(lib.setProjectsFolder(QString())); // back to the standard one
        CHECK(lib.projectsFolder() == ProjectLibrary::standardProjectsFolder());
    }
    {
        // A change without load() first (a project opened from the command
        // line) keeps what's in the file.
        ProjectLibrary first(listFile);
        CHECK(first.setProjectsFolder(folder));
        ProjectLibrary lib(listFile);
        CHECK(lib.noteOpened(loose, t0.addSecs(300)));
        lib.load();
        CHECK(lib.projectsFolder() == folder);
        CHECK(lib.entries().size() == 3);
        CHECK(lib.entries().constFirst().path == loose);
    }
    {
        // A file that isn't a library is ignored, not fatal.
        QFile f(listFile);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{\"format\": \"something else\"}");
        f.close();
        ProjectLibrary lib(listFile);
        lib.load();
        CHECK(lib.projectsFolder() == ProjectLibrary::standardProjectsFolder());
    }
}

void testProjectSummary()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString file = projectfolder::create(tmp.path(), "Trip");
    const QString clipA = tmp.path() + "/a.insv";
    const QString clipB = tmp.path() + "/b.insv";
    const QString song = tmp.path() + "/song.wav";
    writeFile(clipA);
    writeFile(clipB);
    writeFile(song);
    {
        Project p;
        const int a = p.timeline()->appendClip(clipA, 10000, 30.0);
        p.timeline()->appendClip(clipB, 6000, 30.0);
        p.timeline()->setClipTrimOut(a, 4000);
        const int track = p.timeline()->addTrack(Track::Kind::Audio, "Music");
        p.timeline()->addAudioClip(track, song, 20000, 0);
        CHECK(p.save(file));
    }
    ProjectSummary s = ProjectSummary::read(file);
    CHECK(s.name == "Trip");
    CHECK(!s.fileMissing && s.error.isEmpty());
    CHECK(s.clipCount == 2);
    CHECK(s.durationMs == 10000);
    CHECK(s.missingMedia == 0);
    CHECK(s.firstClip == clipA);

    QFile::remove(clipB);
    QFile::remove(song);
    s = ProjectSummary::read(file);
    CHECK(s.missingMedia == 2);

    QFile f(file);
    CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("not json");
    f.close();
    s = ProjectSummary::read(file);
    CHECK(!s.error.isEmpty());

    s = ProjectSummary::read(tmp.path() + "/gone/gone.orp");
    CHECK(s.fileMissing && s.name == "gone");
}

void testMediaPool()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const auto touch = [](const QString &path) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write("x");
    };
    QDir().mkpath(dir + "/media");
    const QString clip = dir + "/media/VID_1.insv";
    const QString song = dir + "/media/song.mp3";
    const QString phone = dir + "/media/phone.mp4";
    const QString unused = dir + "/media/VID_2.insv";
    for (const QString &f : {clip, song, phone, unused})
        touch(f);

    // Whatever goes on the timeline goes into the pool, once.
    Project p;
    MediaPool *pool = p.mediaPool();
    CHECK(pool->count() == 0);
    p.timeline()->appendClip(clip, 20000, 29.97, 3840, 3840);
    p.timeline()->appendClip(clip, 20000, 29.97, 3840, 3840);
    CHECK(pool->count() == 1);
    CHECK(pool->item(clip) && pool->item(clip)->kind == MediaItem::Kind::Clip360 &&
          pool->item(clip)->durationMs == 20000 && pool->item(clip)->width == 3840 &&
          pool->item(clip)->fps == 29.97);
    const int at = p.timeline()->addTrack(Track::Kind::Audio);
    p.timeline()->addAudioClip(at, song, 60000, 0);
    const int vt = p.timeline()->addTrack(Track::Kind::Video);
    p.timeline()->addVideoClip(vt, phone, 5000, 0, 1080, 1920);
    CHECK(pool->count() == 3);
    CHECK(pool->item(song) && pool->item(song)->kind == MediaItem::Kind::Audio);
    CHECK(pool->item(phone) && pool->item(phone)->kind == MediaItem::Kind::Video &&
          pool->item(phone)->height == 1920);
    // Imported, not on the timeline.
    CHECK(pool->add({unused, MediaItem::Kind::Clip360, 7000, 2880, 2880, 59.94}));
    CHECK(pool->count() == 4);

    // Saved with the project, relative to its folder; read back as it was.
    const QString file = dir + "/p.orp";
    CHECK(p.save(file));
    {
        QFile f(file);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(text.contains("\"media\""));
        CHECK(text.contains("\"media/VID_2.insv\""));
    }
    Project q;
    CHECK(q.load(file));
    CHECK(q.mediaPool()->items() == pool->items());
    CHECK(!q.modified());

    // A missing file is flagged on load.
    QFile::remove(unused);
    Project r;
    CHECK(r.load(file));
    CHECK(r.mediaPool()->item(unused) && r.mediaPool()->item(unused)->missing);
    CHECK(r.mediaPool()->item(clip) && !r.mediaPool()->item(clip)->missing);

    // A project without "media" (an older one) lists its timeline's sources.
    {
        QFile f(file);
        CHECK(f.open(QIODevice::ReadOnly));
        QByteArray text = f.readAll();
        f.close();
        const int start = text.indexOf("\"media\":");
        const int end = text.indexOf(']', start);
        CHECK(start > 0 && end > start);
        text.remove(start, end - start + 2); // the array and its comma
        const QString old = dir + "/old.orp";
        QFile o(old);
        CHECK(o.open(QIODevice::WriteOnly));
        o.write(text);
        o.close();
        Project s;
        CHECK(s.load(old));
        CHECK(s.mediaPool()->count() == 3);
        CHECK(!s.mediaPool()->contains(unused));
        CHECK(!s.modified() && !s.canUndo()); // filling the pool isn't an edit
        CHECK(s.mediaPool()->contains(song) && s.mediaPool()->contains(phone));
    }

    // Pool edits mark the project modified and are undone like any edit.
    Project u;
    CHECK(u.mediaPool()->add({song, MediaItem::Kind::Audio, 60000}));
    CHECK(u.modified() && u.canUndo());
    u.undo();
    CHECK(u.mediaPool()->count() == 0);
    u.redo();
    CHECK(u.mediaPool()->contains(song));
    u.mediaPool()->remove({song});
    CHECK(u.mediaPool()->count() == 0);
    u.newProject();
    CHECK(u.mediaPool()->count() == 0 && !u.modified());

    // Relink: a missing file found somewhere else.
    {
        const QString moved = dir + "/moved";
        QDir().mkpath(moved);
        const QString oldClip = dir + "/media/gone.insv"; // never existed
        const QString newClip = moved + "/gone.insv";
        touch(newClip);
        Project v;
        const int a = v.timeline()->appendClip(oldClip, 20000, 29.97, 3840, 3840);
        v.timeline()->setClipMediaMissing(a, true);
        const int b = v.timeline()->appendClip(clip, 20000, 29.97, 3840, 3840);
        const int t = v.timeline()->addTrack(Track::Kind::Audio);
        v.timeline()->addAudioClip(t, oldClip, 20000, 0); // its sound, detached
        CHECK(v.mediaPool()->item(oldClip) && v.mediaPool()->item(oldClip)->missing);
        v.undo(); v.redo(); // settle the adds into their own step
        const int before = v.mediaPool()->indexOf(oldClip);

        CHECK(v.relinkMedia(oldClip, {newClip, MediaItem::Kind::Clip360, 20000, 3840, 3840,
                                      29.97}) == 2);
        CHECK(!v.mediaPool()->contains(oldClip));
        CHECK(v.mediaPool()->indexOf(newClip) == before); // in its place
        CHECK(!v.mediaPool()->item(newClip)->missing);
        CHECK(v.timeline()->clipAt(0).sourcePath == newClip);
        CHECK(!v.timeline()->clipAt(0).mediaMissing);
        CHECK(v.timeline()->clipAt(1).sourcePath == clip);
        CHECK(v.timeline()->trackAt(0).clips.front().sourcePath == newClip);
        CHECK(!v.timeline()->trackAt(0).clips.front().mediaMissing);
        CHECK(v.mediaPool()->count() == 2);
        Q_UNUSED(b);

        // One undo step back.
        v.undo();
        CHECK(v.timeline()->clipAt(0).sourcePath == oldClip);
        CHECK(v.mediaPool()->contains(oldClip) && !v.mediaPool()->contains(newClip));
        v.redo();
        CHECK(v.timeline()->clipAt(0).sourcePath == newClip);

        // Relinked to a file the pool has already: the old entry just goes.
        Project w;
        w.mediaPool()->add({oldClip, MediaItem::Kind::Clip360, 20000});
        w.mediaPool()->add({newClip, MediaItem::Kind::Clip360, 20000});
        CHECK(w.relinkMedia(oldClip, {newClip, MediaItem::Kind::Clip360, 20000}) == 0);
        CHECK(w.mediaPool()->count() == 1 && w.mediaPool()->contains(newClip));
        // Not in the pool: nothing happens.
        CHECK(!w.mediaPool()->relink(dir + "/nothing.insv", {newClip}));
    }
}

int main()
{
    testClipItemTrim();
    testClipItemReframe();
    testInsetKeyTrack();
    testStreamRoles();
    testClipItemColor();
    testTimelineMapping();
    testClipSpeed();
    testKeyboardNavigation();
    testKeyNavigation();
    testTimelineStructure();
    testTimelineSplit();
    testStabConfig();
    testProjectJson();
    testProjectUndoRedo();
    testPerClipStabilization();
    testExportBitrate();
    testAudioTracks();
    testLinkedAudio();
    testClipSound();
    testProjectConvertSound();
    testOverlayAudio();
    testTrackGroups();
    testOwnSoundTracks();
    testProjectTracksJson();
    testVideoTracks();
    testProjectVideoTracksJson();
    testMediaPool();
    testProjectFolder();
    testProjectLibrary();
    testProjectSummary();

    if (g_failures == 0)
        std::printf("core_model_test: all checks passed\n");
    else
        std::printf("core_model_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
