// Timeline, the main track: stream roles, mapping timeline time to clips,
// clip speed, insert / append / remove / move, split, keyboard navigation
// between cuts and keys, and the track queries behind adding files.
// Standalone (no event loop: same-thread signals fire inside emit); the
// exit code is the failure count.

#include "support/ModelTest.h"

namespace {

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

// Where added files go and what an audio clip links to: the first track
// with room, the nearest clip of the same footage, an overlay's own sound.
void testTrackQueries()
{
    Timeline t;
    t.appendClip("/v/ride.insv", 20000, 30.0);
    const int music = t.addTrack(Track::Kind::Audio, "Music");
    const int more = t.addTrack(Track::Kind::Audio, "More");
    t.addAudioClip(music, "/m/a.wav", 5000, 1000); // 1 s .. 6 s
    auto anyAudio = [](const Track &) { return true; };
    CHECK(t.freeTrackFor(0, 1000, false, anyAudio) != 0);
    CHECK(t.freeTrackFor(2000, 3000, false, [&](const Track &tr) { return tr.id == music; }) == 0);
    CHECK(t.freeTrackFor(2000, 3000, false, [&](const Track &tr) {
              return tr.id == music || tr.id == more;
          }) == more);
    CHECK(t.freeTrackFor(6000, 7000, false, [&](const Track &tr) { return tr.id == music; }) ==
          music); // touching the end is room
    CHECK(t.freeTrackFor(0, 1000, true, anyAudio) == 0); // no overlay track yet

    // An overlay of a file with sound: its audio track is made, and asking
    // again gives the same clip.
    const int ov = t.addTrack(Track::Kind::Video, "V1");
    CHECK(t.freeTrackFor(0, 1000, true, anyAudio) == ov);
    const int clip = t.addVideoClip(ov, "/v/phone.mp4", 4000, 2000, 1080, 1920);
    const int sound = t.addOverlaySound(clip);
    CHECK(sound != 0 && t.addOverlaySound(clip) == sound);
    int soundTrack = 0;
    const TrackClip *s = t.trackClipById(sound, &soundTrack);
    CHECK(s && s->linkedClipId == clip && s->startMs == 2000);
    CHECK(soundTrack == t.audioTrackId(ov));
    CHECK(t.addOverlaySound(sound) == 0); // not an overlay clip

    // Unlinked, it links back to its overlay; a main-track clip's sound to
    // that clip.
    t.linkAudioClip(sound, 0);
    if (const TrackClip *u = t.trackClipById(sound))
        CHECK(t.linkTargetFor(*u) == clip);
    TrackClip stray;
    stray.sourcePath = "/v/ride.insv";
    stray.sourceDurationMs = 20000;
    stray.trimInMs = 5000;
    stray.trimOutMs = 8000;
    stray.startMs = 5000;
    CHECK(t.linkTargetFor(stray) == t.clipAt(0).id);
    stray.sourcePath = "/v/other.insv";
    CHECK(t.linkTargetFor(stray) == 0);
}

} // namespace

int main()
{
    testStreamRoles();
    testTimelineMapping();
    testClipSpeed();
    testKeyboardNavigation();
    testKeyNavigation();
    testTimelineStructure();
    testTimelineSplit();
    testTrackQueries();
    return modeltest::finish("timeline_test");
}
