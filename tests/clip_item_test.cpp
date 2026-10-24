// ClipItem: trims (the clamp and minimum gap), reframe keys (snap, delete,
// sampling), overlay look keys and the colour grade.
// Standalone (no event loop: same-thread signals fire inside emit); the
// exit code is the failure count.

#include "support/ModelTest.h"

namespace {

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

    CHECK(c.upsertReframeKey(1000, {0.5f, 0.0f, 0.0f, 1.3f})); // {yaw,pitch,roll,fov}
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

// Moves start and stop gently (keytrack::eased's softStart / softEnd): the
// speed ramps from and to 0, never jumps; carrying on through a key, it
// doesn't slow down.
void testSoftMoves()
{
    using keytrack::Ease;
    using keytrack::eased;
    for (Ease e : {Ease::Linear, Ease::Out, Ease::In})
        for (int soft = 0; soft < 4; ++soft) {
            const bool s0 = soft & 1, s1 = soft & 2;
            CHECK(eased(e, 0.0f, s0, s1) == 0.0f && eased(e, 1.0f, s0, s1) == 1.0f);
            float prev = 0.0f;
            bool rising = true;
            for (int i = 1; i <= 100; ++i) {
                const float v = eased(e, i / 100.0f, s0, s1);
                rising &= v >= prev;
                prev = v;
            }
            CHECK(rising);
        }
    // From rest: barely moving at first (a jump would be 1x pace or more).
    CHECK(eased(Ease::Linear, 0.01f, true, false) / 0.01f < 0.1f);
    CHECK(eased(Ease::Out, 0.01f, true, false) / 0.01f < 0.1f);
    // ... and into rest.
    CHECK((1.0f - eased(Ease::Linear, 0.99f, false, true)) / 0.01f < 0.1f);
    CHECK((1.0f - eased(Ease::In, 0.99f, false, true)) / 0.01f < 0.1f);
    // The ramp is an S-curve: no acceleration as it leaves rest, so the
    // distance grows with t^3 there (an even ramp's with t^2: ratio 1/4) ...
    const float ramp = keytrack::kSoftRamp;
    CHECK(std::abs(eased(Ease::Linear, 0.002f, true, false) /
                   eased(Ease::Linear, 0.004f, true, false) - 0.125f) < 0.01f);
    // ... and it covers what an even ramp would: by its end, (ramp/2) of the
    // (1 - ramp/2) the move takes in all.
    CHECK(std::abs(eased(Ease::Linear, ramp, true, false) - (ramp / 2) / (1 - ramp / 2)) < 1e-6f);
    // Soft at both ends, Linear is symmetric; in between at its pace.
    CHECK(std::abs(eased(Ease::Linear, 0.5f, true, true) - 0.5f) < 1e-6f);
    CHECK(std::abs(eased(Ease::Linear, 0.1f, true, true) +
                   eased(Ease::Linear, 0.9f, true, true) - 1.0f) < 1e-6f);

    // Through a key: a pan 0 -> 1 -> 2 at an even pace eases out of the first
    // key and into the last, and doesn't stop at the middle one.
    ReframeTrack r;
    r.upsert(0, {0.0f, 0.0f, 0.0f, 1.0f}, 40);
    r.upsert(1000, {1.0f, 0.0f, 0.0f, 1.0f}, 40);
    r.upsert(2000, {2.0f, 0.0f, 0.0f, 1.0f}, 40);
    const auto speed = [&r](qint64 ms) { return (r.sample(ms + 5).yaw - r.sample(ms - 5).yaw) / 10; };
    CHECK(speed(10) < 0.1f * speed(1000));
    CHECK(speed(1990) < 0.1f * speed(1000));
    CHECK(speed(1000) > 0.001f);                                    // no stop at the middle key
    CHECK(std::abs(speed(995) - speed(1005)) < 0.05f * speed(1000)); // nor a jump
    // A hold (the same view twice) stops the motion: leaving it starts from rest.
    r.upsert(3000, {2.0f, 0.0f, 0.0f, 1.0f}, 40);
    r.upsert(4000, {3.0f, 0.0f, 0.0f, 1.0f}, 40);
    CHECK(speed(3010) < 0.1f * speed(3500));
}

// Easing (keytrack::): a key's ease shapes the move to the next key, the
// segment is found from anywhere in it or near its key, there's none before
// the first / at the last key, and re-authoring a key keeps its ease.
void testKeyEasing()
{
    using keytrack::Ease;
    CHECK(keytrack::eased(Ease::Linear, 0.25f) == 0.25f);
    CHECK(keytrack::eased(Ease::Out, 0.5f) == 0.75f); // ahead: fast, then slow
    CHECK(keytrack::eased(Ease::In, 0.5f) == 0.25f);  // behind: slow, then fast
    for (Ease e : {Ease::Linear, Ease::Out, Ease::In})
        CHECK(keytrack::eased(e, 0.0f) == 0.0f && keytrack::eased(e, 1.0f) == 1.0f);
    CHECK(keytrack::easeFromName(keytrack::easeName(Ease::Out)) == Ease::Out);
    CHECK(keytrack::easeFromName(keytrack::easeName(Ease::In)) == Ease::In);
    CHECK(keytrack::easeFromName("") == Ease::Linear);
    CHECK(keytrack::easeFromName("bounce") == Ease::Linear); // a newer build's

    ReframeTrack r;
    r.upsert(1000, {0.0f, 0.0f, 0.0f, 1.0f}, 40);
    r.upsert(2000, {1.0f, 0.0f, 0.0f, 2.0f}, 40);
    r.upsert(3000, {2.0f, 0.0f, 0.0f, 2.0f}, 40);
    CHECK(keytrack::segmentAt(r.keys(), 500, 150) == -1);  // before the first key
    CHECK(keytrack::segmentAt(r.keys(), 1500, 150) == 0);
    CHECK(keytrack::segmentAt(r.keys(), 1900, 150) == 1);  // near the 2000 key: its segment
    CHECK(keytrack::segmentAt(r.keys(), 3000, 150) == -1); // the last key has none
    CHECK(keytrack::segmentAt(r.keys(), 2950, 150) == -1);
    CHECK(!r.setEase(3000, 150, Ease::Out));
    CHECK(r.setEase(1500, 150, Ease::Out));
    CHECK(!r.setEase(1000, 150, Ease::Out)); // already so
    CHECK(r.keys()[0].ease == Ease::Out && r.keys()[1].ease == Ease::Linear);
    // Out from the first key: it starts from rest (softened), so at half
    // time it's 2/3 of the way, not 3/4.
    const ReframePose mid = r.sample(1500);
    CHECK(std::abs(mid.yaw - 2.0f / 3.0f) < 1e-5f && std::abs(mid.fov - 5.0 / 3.0) < 1e-5);
    // The next segment stays linear, from rest (Out ended there) to rest (the
    // last key): symmetric, half way at half time.
    CHECK(std::abs(r.sample(2500).yaw - 1.5f) < 1e-5f);
    CHECK(r.sample(1000).yaw == 0.0f && r.sample(2000).yaw == 1.0f); // keys hit exactly
    r.upsert(1010, {0.1f, 0.0f, 0.0f, 1.0f}, 40); // re-authored: the ease stays
    CHECK(r.count() == 3 && r.keys()[0].ease == Ease::Out);
    r.upsert(1500, {0.5f, 0.0f, 0.0f, 1.0f}, 40); // a new key in the segment: linear
    CHECK(r.keys()[1].timeMs == 1500 && r.keys()[1].ease == Ease::Linear);

    InsetKeyTrack k;
    inset::Look a, b;
    a.centerX = 0.2f;
    b.centerX = 0.6f;
    k.upsert(0, a, 40);
    k.upsert(1000, b, 40);
    const InsetKeyTrack linear = k;
    CHECK(k.setEase(500, 150, Ease::In));
    CHECK(!(k == linear)); // the ease is part of the track's value (undo, change checks)
    // In, into the last key (soft end): 1/3 of the way at half time.
    CHECK(std::abs(k.sample(500).centerX - (0.2f + 0.4f / 3.0f)) < 1e-5f);
}

// The field of view and the distortion between keys: two Auto keys stay
// Auto (the blended angle picks the distortion), a fixed one blends with
// what an Auto one shows.
void testWideViewKeys()
{
    constexpr double deg = 3.14159265358979 / 180.0;
    ReframeTrack r;
    ReframePose narrow, wide;
    narrow.fov = 80.0 * deg;
    wide.fov = 180.0 * deg;
    r.upsert(0, narrow, 40);
    r.upsert(1000, wide, 40);
    ReframePose mid = r.sample(500);
    CHECK(std::abs(mid.fov - 130.0 * deg) < 1e-9 && mid.distortionIsAuto());
    CHECK(std::abs(view::distortionOf(mid) - view::autoDistortion(130.0 * deg)) < 1e-12);

    wide.distortion = 0.6f; // fixed; narrow's Auto shows 0
    r.upsert(1000, wide, 40);
    mid = r.sample(500);
    CHECK(!mid.distortionIsAuto() && std::abs(mid.distortion - 0.3f) < 1e-6f);
    CHECK(r.sample(1000).distortion == 0.6f && r.sample(0).distortionIsAuto());
    // Keys that differ only in distortion are a move.
    ReframeTrack s;
    ReframePose flat = narrow;
    flat.distortion = 0.0f;
    ReframePose bent = narrow;
    bent.distortion = 1.0f;
    s.upsert(0, flat, 40);
    s.upsert(1000, bent, 40);
    CHECK(std::abs(s.sample(500).distortion - 0.5f) < 1e-6f);
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

} // namespace

int main()
{
    testClipItemTrim();
    testClipItemReframe();
    testInsetKeyTrack();
    testKeyEasing();
    testSoftMoves();
    testWideViewKeys();
    testClipItemColor();
    return modeltest::finish("clip_item_test");
}
