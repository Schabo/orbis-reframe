// Timeline, the extra tracks: a clip's sound on its stream tracks, audio
// tracks, linked audio, overlay (video) tracks and their sound, track groups
// and the video tracks' own sound tracks.
// Standalone (no event loop: same-thread signals fire inside emit); the
// exit code is the failure count.

#include "support/ModelTest.h"

#include <cmath>

namespace {

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
    // A split gives each half the keys inside it.
    const int right = tl.splitTrackClip(c, 5000); // source 2000
    CHECK(right > 0);
    CHECK(tl.trackClipById(right)->insetKeys.keys().size() == 1 &&
          tl.trackClipById(right)->insetKeys.keys()[0].timeMs == 3000);
    CHECK(tl.trackClipById(c)->insetKeys.keys().size() == 1 &&
          tl.trackClipById(c)->insetKeys.keys()[0].timeMs == 1000);
    // A trim removes the keys it leaves out; with none left, the look stays
    // as the clip showed it at its in-point (the 3000 key's, k0 since the
    // snap), not the static look it had before its keys.
    tl.setTrackClipTrimIn(right, 3500);
    pc = tl.trackClipById(right);
    CHECK(pc->insetKeys.isEmpty() && pc->look.centerX == 0.2f);
    tl.removeTrackClip(right);
    tl.removeTrackClipInsetKeyNear(c, 1100);
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

// A title: a video-track clip with no file, never missing, with room to
// extend either edge, its own style, and no sound to put on a track.
void testTitles()
{
    Timeline tl;
    tl.appendClip("/v/a.insv", 60000, 30.0);
    const int a = tl.addTrack(Track::Kind::Audio);
    const int v = tl.addTrack(Track::Kind::Video);
    title::Style style;
    style.text = "Hello";
    CHECK(tl.addTitleClip(a, 1000, style) == 0); // not on an audio track
    const int id = tl.addTitleClip(v, 2000, style);
    const TrackClip *c = tl.trackClipById(id);
    CHECK(c && c->isTitle() && c->title->text == "Hello");
    CHECK(c->sourcePath.isEmpty() && !c->mediaMissing);
    CHECK(c->startMs == 2000 && c->timelineDurationMs() == title::kDefaultSpanMs);
    CHECK(c->trimInMs > 0 && c->trimOutMs < c->sourceDurationMs); // either edge can grow
    CHECK(c->look.centerX == 0.5f && c->look.centerY == 0.5f &&
          c->look.width == title::kDefaultSize);

    // Its head dragged out keeps the end where it was.
    tl.setTrackClipTrimIn(id, c->trimInMs - 1000);
    c = tl.trackClipById(id);
    CHECK(c->startMs == 1000 && c->timelineDurationMs() == title::kDefaultSpanMs + 1000);

    int changes = 0;
    QObject::connect(&tl, &Timeline::tracksChanged, [&] { ++changes; });
    style.text = "Hello\nworld";
    style.rgb = 0xff2040;
    tl.setTrackClipTitle(id, style);
    CHECK(changes == 1 && tl.trackClipById(id)->title->text == "Hello\nworld" &&
          tl.trackClipById(id)->title->rgb == 0xff2040);
    tl.setTrackClipTitle(id, style); // no change, no signal
    CHECK(changes == 1);

    // Out-of-range look values are clamped.
    style.lineSpacing = 9.0f;
    style.box.cornerRadius = -1.0f;
    tl.setTrackClipTitle(id, style);
    CHECK(changes == 2 &&
          tl.trackClipById(id)->title->lineSpacing == title::Style::kMaxLineSpacing &&
          tl.trackClipById(id)->title->box.cornerRadius == 0.0f);
    style = *tl.trackClipById(id)->title;

    // A new alignment with a shift moves the position and every key by that
    // many text sizes, in one change; the keys keep their times and sizes.
    {
        const TrackClip before = *tl.trackClipById(id);
        inset::Look k = before.look;
        k.width = 0.2f;
        tl.upsertTrackClipInsetKey(id, before.trimInMs + 500, k);
        const float keyX = tl.trackClipById(id)->insetKeys.keys().first().look.centerX;
        const int at = changes;
        style.align = title::Align::Left;
        tl.setTrackClipTitle(id, style, -1.5);
        const TrackClip &after = *tl.trackClipById(id);
        CHECK(changes == at + 1 && after.title->align == title::Align::Left);
        const float wantX = before.look.centerX - 1.5f * before.look.width;
        CHECK(std::abs(after.look.centerX - wantX) < 1e-5f);
        const inset::Look key = after.insetKeys.keys().first().look;
        CHECK(after.insetKeys.count() == 1 && key.width == 0.2f &&
              std::abs(key.centerX - (keyX - 1.5f * 0.2f)) < 1e-5f);
        tl.clearTrackClipInsetKeys(id, before.trimInMs);
        tl.setTrackClipInset(id, before.look, before.frame);
        style.align = title::Align::Centre;
        tl.setTrackClipTitle(id, style);
    }

    CHECK(tl.addOverlaySound(id) == 0); // nothing to hear

    // How it comes and goes: clamped, one change, none when the same.
    inset::Motion m;
    m.in = {inset::Motion::Kind::Slide, inset::Motion::Side::Top, 50};
    m.out = {inset::Motion::Kind::Pop, inset::Motion::Side::Left, 800};
    {
        const int at = changes;
        tl.setTrackClipMotion(id, m);
        tl.setTrackClipMotion(id, m.clamped());
        CHECK(changes == at + 1 && tl.trackClipById(id)->motion.in.ms == inset::Motion::kMinMs &&
              tl.trackClipById(id)->motion.out == m.out);
    }

    // A split keeps the style on both halves; the left one comes on, the
    // right one goes off.
    const int right = tl.splitTrackClip(id, 3000);
    CHECK(right && tl.trackClipById(right)->isTitle() &&
          tl.trackClipById(right)->title->text == "Hello\nworld");
    CHECK(tl.trackClipById(id)->motion.in.kind == inset::Motion::Kind::Slide &&
          tl.trackClipById(id)->motion.out.kind == inset::Motion::Kind::None);
    CHECK(tl.trackClipById(right)->motion.in.kind == inset::Motion::Kind::None &&
          tl.trackClipById(right)->motion.out == m.out);
    // A preset's motion comes with it on add.
    const int moving = tl.addTitleClip(v, 20000, style, 3000, title::defaultLook(), m);
    CHECK(moving && tl.trackClipById(moving)->motion == m.clamped());

    // A file going missing doesn't touch it.
    tl.setSourcesMissing({{QString(), true}});
    CHECK(!tl.trackClipById(id)->mediaMissing);
}

} // namespace

int main()
{
    testAudioTracks();
    testLinkedAudio();
    testClipSound();
    testOverlayAudio();
    testTrackGroups();
    testOwnSoundTracks();
    testVideoTracks();
    testTitles();
    return modeltest::finish("timeline_tracks_test");
}
