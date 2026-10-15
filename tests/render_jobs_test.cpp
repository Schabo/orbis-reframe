// src/ui/RenderJobs: the edit model as playback, the export and a
// screenshot are handed it -- the audio mix, the overlays (what's left out,
// in which order), a main-track clip's export job, and the suggested file
// names. No GPU, no media.

#include "RenderJobs.h"

#include "PrivacyBlur.h"
#include "../core/ClipItem.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>

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

bool near(double a, double b, double eps = 1e-4) { return std::abs(a - b) < eps; }

void testMix()
{
    Timeline t;
    const int music = t.addTrack(Track::Kind::Audio, "Music");
    const int muted = t.addTrack(Track::Kind::Audio, "Muted");
    t.setTrackAudio(music, -6.0, false);
    t.setTrackAudio(muted, 0.0, true);
    const int a = t.addAudioClip(music, "/m/a.wav", 5000, 1000);
    t.setAudioClipLevel(a, -6.0, false);
    t.setTrackClipFades(a, 500, 250);
    const int quiet = t.addAudioClip(music, "/m/quiet.wav", 1000, 8000);
    t.setAudioClipLevel(quiet, 0.0, true);
    t.addAudioClip(music, "/m/gone.wav", 1000, 10000);
    t.addAudioClip(muted, "/m/b.wav", 1000, 0);
    t.setSourcesMissing({{"/m/gone.wav", true}});

    const std::vector<insv::MixClip> mix = renderjobs::mixClips(t);
    CHECK(mix.size() == 1); // muted track, muted clip and missing file left out
    if (mix.size() == 1) {
        const insv::MixClip &m = mix[0];
        CHECK(m.path == "/m/a.wav");
        CHECK(m.startFrame == 1000 * 48 && m.lengthFrames == 5000 * 48 && m.inFrame == 0);
        CHECK(near(m.gain, std::pow(10.0, -12.0 / 20.0))); // clip + track
        CHECK(m.fadeInFrames == 500 * 48 && m.fadeOutFrames == 250 * 48);
    }
}

void testOverlays()
{
    Timeline t;
    const int v1 = t.addTrack(Track::Kind::Video, "V1");
    const int v2 = t.addTrack(Track::Kind::Video, "V2");
    const int hidden = t.addTrack(Track::Kind::Video, "Hidden");
    t.setTrackAudio(hidden, 0.0, true);
    const int c1 = t.addVideoClip(v1, "/v/one.mp4", 4000, 0, 1920, 1080);
    const int c2 = t.addVideoClip(v2, "/v/two.mp4", 4000, 500, 1080, 1920);
    t.addVideoClip(hidden, "/v/hidden.mp4", 4000, 0, 1920, 1080);
    t.addVideoClip(v2, "/v/gone.mp4", 1000, 6000, 1920, 1080);
    t.setSourcesMissing({{"/v/gone.mp4", true}});
    inset::Look keyed;
    keyed.centerX = 0.25f;
    t.upsertTrackClipInsetKey(c2, 1000, keyed);

    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t);
    CHECK(feed.size() == 2 && jobs.size() == 2);
    if (feed.size() != 2 || jobs.size() != 2)
        return;
    // Bottom-most first: the track listed last draws first.
    const int i1 = t.trackIndexOfId(v1), i2 = t.trackIndexOfId(v2);
    const int first = i1 > i2 ? c1 : c2;
    CHECK(feed[0].id == first);
    CHECK(jobs[0].path == (first == c1 ? "/v/one.mp4" : "/v/two.mp4"));
    for (size_t k = 0; k < feed.size(); ++k) {
        CHECK(jobs[k].path == feed[k].path.toStdString());
        CHECK(jobs[k].startMs == feed[k].startMs && near(jobs[k].aspect, feed[k].aspect));
    }
    const size_t k2 = feed[0].id == c2 ? 0 : 1;
    CHECK(near(feed[k2].aspect, 1080.0 / 1920.0));
    CHECK(near(jobs[k2].lookAt(1000).centerX, 0.25f)); // its key, sampled
    CHECK(jobs[1 - k2].lookAt(1000).centerX != 0.25f); // the other keeps its own look
}

void testClipJob(const QString &dir)
{
    Timeline t;
    const int id = t.appendClip("/c/ride.insv", 20000, 30.0);
    t.setClipTrimIn(id, 2000);
    t.setClipTrimOut(id, 9000);
    t.setClipSpeed(id, 2.0);
    ColorGrade g;
    g.exposureEv = 1.0f;
    t.setClipColor(id, g);
    t.setSourceSeamAdjust("/c/ride.insv", {-0.5, 0.25, 0.1}, 2);
    PrivacyBlur privacy;
    privacy.scans().setDirectory(dir);

    const ClipItem &c = t.clipAt(0);
    ReframeExportController::ClipJob job = renderjobs::clipJob(c, t, 2, privacy);
    CHECK(job.clipPath == "/c/ride.insv" && job.trimInMs == 2000 && job.trimOutMs == 9000);
    CHECK(job.speed == 2.0 && job.colorGrade[0] == 1.0f);
    CHECK(job.seamAdjustDeg[1] == 0.25);
    CHECK(!job.stab);    // stabilization off
    CHECK(!job.poseAt);  // no keys: looking straight ahead
    CHECK(!job.blur);    // no blur
    job = renderjobs::clipJob(c, t, 1, privacy);
    CHECK(job.seamAdjustDeg[1] == 0.0); // measured under another guard

    t.upsertClipReframeKey(id, 3000, {0.5f, 0.0f, 0.0f, 1.2f});
    job = renderjobs::clipJob(t.clipAt(0), t, 2, privacy);
    CHECK(job.poseAt);
    if (job.poseAt) {
        float pose[4] = {};
        job.poseAt(5000, pose);
        CHECK(near(pose[0], 0.5f) && near(pose[3], 1.2f));
    }
}

void testSuggestedPaths(const QString &dir)
{
    CHECK(renderjobs::suggestedExportPath(QString()) == QDir::homePath() + "/timeline.mp4");
    const QString file = projectfolder::create(dir, "Ride");
    CHECK(!file.isEmpty());
    const QString first = renderjobs::suggestedExportPath(file);
    CHECK(first == QDir(projectfolder::exportsDir(file)).filePath("Ride.mp4"));
    QFile f(first);
    CHECK(f.open(QIODevice::WriteOnly));
    f.close();
    CHECK(renderjobs::suggestedExportPath(file) ==
          QDir(projectfolder::exportsDir(file)).filePath("Ride-2.mp4"));

    const QString shot = renderjobs::suggestedScreenshotPath(file, "/c/ride.insv", 62003);
    CHECK(shot == QDir(projectfolder::screenshotsDir(file)).filePath("Ride_01m02s003.png"));
    CHECK(renderjobs::suggestedScreenshotPath(QString(), "/c/ride.insv", -5)
              .endsWith("/ride_00m00s000.png")); // unsaved: named after the clip
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    testMix();
    testOverlays();
    testClipJob(tmp.path());
    testSuggestedPaths(tmp.path());
    if (g_failures == 0)
        std::printf("render_jobs_test: all checks passed\n");
    else
        std::printf("render_jobs_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
