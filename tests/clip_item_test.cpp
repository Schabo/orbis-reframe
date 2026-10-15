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
    testClipItemColor();
    return modeltest::finish("clip_item_test");
}
