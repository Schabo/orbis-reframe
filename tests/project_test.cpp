// Project and its files: the .orp JSON round trip (clips, tracks, overlays),
// converting older projects, undo / redo, the export bitrate, autosave and
// recovery, measurements written in place, project folders, the library,
// project summaries and the media pool.
// Standalone (no event loop: same-thread signals fire inside emit); the
// exit code is the failure count.

#include "support/ModelTest.h"

#include "TitleClock.h"
#include "LensCalibrationStore.h"

#include <QCoreApplication>
#include <QStandardPaths>

namespace {

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
        p.timeline()->setClipReframeEase(a, 3000, keytrack::Ease::Out); // 2000's segment
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
        p.timeline()->setClipPlateScan(a, "3f9c0a7d21be"); // its scan
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
        p.timeline()->setSourceSeamCorrection(insideSrc, {{-0.55, 0.05, -0.05}, {0.0012, -0.0004}, 0.9992},
                                              2); // seam fine-tune
        CHECK(p.modified());

        stitch::PreviewStitchConfig sc;
        sc.width = 2560;
        sc.height = 1280;
        p.setPreviewStitch(sc); // the preview's part; lens guards are per clip

        CHECK(p.previewStitch().width == 2560);
        CHECK(p.previewStitch() == sc);

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
        out.tenBit = true;
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
        CHECK(a.reframe.keys()[1].pose.fov == 0.9f); // exactly: saved as a double
        CHECK(a.reframe.keys()[1].pose.distortionIsAuto());
        CHECK(a.reframe.keys()[0].ease == keytrack::Ease::Out);
        CHECK(a.reframe.keys()[1].ease == keytrack::Ease::Linear);
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
        CHECK(a.plateScan == "3f9c0a7d21be" && b.plateScan.isEmpty());
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
        CHECK(tl->clipById(a2)->seamCorrection.lens1RotDeg[0] == -0.55);
        CHECK(tl->clipById(a2)->seamCorrection.lens1Centre[0] == 0.0012 &&
              tl->clipById(a2)->seamCorrection.lens1Centre[1] == -0.0004);
        CHECK(tl->clipById(a2)->seamCorrection.radialScale == 0.9992);

        const stitch::PreviewStitchConfig &sc = p.previewStitch();
        CHECK(sc.width == 2560 && sc.height == 1280);

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
        CHECK(out.tenBit);

    }

    // missing blocks load with defaults
    {
        const QString bare = projDir + "/bare.orp";
        writeFile(bare, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":8,"timeline":{"clips":[]}})"));
        Project p;
        QString err;
        CHECK(p.load(bare, &err));
        stitch::PreviewStitchConfig older; // a file that doesn't say: the old stitcher
        older.method = Project::kOlderFileStitchMethod;
        CHECK(p.previewStitch() == older);
        CHECK(p.output() == OutputSettings{});
        CHECK(p.output().fps == 0.0 && p.output().effectiveFps(29.97) == 29.97);
        CHECK(!p.output().tenBit); // no bitDepth: 8-bit, as before it existed
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
        useOldStitcher(*p.timeline());
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

    // Undo / redo keep the tracks' ids (what the view keys by them, a folded
    // overlay track, survives); a new track gets an id none had; the saved
    // file has no ids.
    {
        Project p;
        p.timeline()->appendClip(video, 10000, 30.0);
        const int o = p.timeline()->addTrack(Track::Kind::Video);
        const int a = p.timeline()->addTrack(Track::Kind::Audio);
        p.undo(); // settle
        p.redo();
        CHECK(p.timeline()->trackById(o) && p.timeline()->trackById(a));
        const int b = p.timeline()->addTrack(Track::Kind::Audio);
        p.undo();
        CHECK(p.timeline()->trackCount() == 2 && !p.timeline()->trackById(b));
        CHECK(p.timeline()->trackById(o) && p.timeline()->trackById(a));
        const int c = p.timeline()->addTrack(Track::Kind::Audio);
        CHECK(c != o && c != a && c != b);
        CHECK(p.save(projFile));
        QFile f(projFile);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonArray tracks = QJsonDocument::fromJson(f.readAll())
                                      .object()["timeline"].toObject()["tracks"].toArray();
        CHECK(tracks.size() == 3);
        for (const QJsonValue &jt : tracks)
            CHECK(!jt.toObject().contains("id"));
    }
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
        l.centerX = 0.6f;
        p.timeline()->upsertTrackClipInsetKey(c, 4000, l);
        p.timeline()->setTrackClipInsetEase(c, 2000, keytrack::Ease::In);
        p.timeline()->setTrackAudio(t, 0.0, true); // hidden
        CHECK(p.save(projFile));
    }
    {
        QFile f(projFile);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(text.contains("\"kind\":\"video\"") && text.contains("\"pip/cam.mp4\""));
        CHECK(text.contains("\"borderColor\":\"#00ff80\""));
        CHECK(text.count("\"ease\"") == 1 && text.contains("\"ease\":\"easeIn\""));
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
    CHECK(c.insetKeys.count() == 2 && c.insetKeys.keys()[0].timeMs == 2000);
    CHECK(c.insetKeys.keys()[0].look.opacity == 0.25f);
    CHECK(c.insetKeys.keys()[0].ease == keytrack::Ease::In);
    CHECK(c.insetKeys.keys()[1].ease == keytrack::Ease::Linear);
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

    // Files going and coming back while the project is open: the flags
    // follow the disk, and it isn't an edit.
    {
        const QString usb = dir + "/usb";
        QDir().mkpath(usb);
        const QString vid = usb + "/VID_9.insv";
        const QString mic = usb + "/mic.wav";
        const QString pip = usb + "/pip.mp4";
        for (const QString &f : {vid, mic, pip})
            touch(f);
        Project x;
        x.timeline()->appendClip(vid, 20000, 29.97, 3840, 3840);
        const int at = x.timeline()->addTrack(Track::Kind::Audio);
        x.timeline()->addAudioClip(at, mic, 10000, 0);
        const int vt = x.timeline()->addTrack(Track::Kind::Video);
        x.timeline()->addVideoClip(vt, pip, 5000, 0, 1920, 1080);
        CHECK(x.mediaPool()->add({unused, MediaItem::Kind::Clip360, 7000})); // gone already
        CHECK(x.mediaPool()->item(unused)->missing);
        CHECK(x.save(dir + "/x.orp")); // not modified from here
        const bool couldUndo = x.canUndo();
        CHECK(x.recheckMissingMedia().isEmpty());

        int poolSignals = 0, timelineSignals = 0;
        QObject::connect(x.mediaPool(), &MediaPool::missingChanged, [&] { ++poolSignals; });
        QObject::connect(x.timeline(), &Timeline::mediaMissingChanged, [&] { ++timelineSignals; });
        const QString away = dir + "/away";
        QDir().rename(usb, away); // the drive unplugged
        Project::MediaRecheck changed = x.recheckMissingMedia();
        CHECK(changed.gone.size() == 3 && changed.gone.contains(vid) &&
              changed.gone.contains(mic) && changed.gone.contains(pip));
        CHECK(changed.back.isEmpty() && changed.replaced.isEmpty());
        CHECK(poolSignals == 1 && timelineSignals == 1);
        CHECK(x.mediaPool()->item(vid)->missing && x.mediaPool()->item(pip)->missing);
        CHECK(x.timeline()->clipAt(0).mediaMissing);
        const Track *audio = x.timeline()->trackById(at);
        const Track *video = x.timeline()->trackById(vt);
        CHECK(audio && audio->clips.front().mediaMissing);
        CHECK(video && video->clips.front().mediaMissing);
        CHECK(!x.modified() && x.canUndo() == couldUndo);
        CHECK(x.recheckMissingMedia().isEmpty()); // nothing new: no signals
        CHECK(poolSignals == 1 && timelineSignals == 1);

        QDir().rename(away, usb); // and plugged back in
        touch(unused);            // a file that was never there turns up
        changed = x.recheckMissingMedia();
        CHECK(changed.back.size() == 4 && changed.back.contains(vid) &&
              changed.back.contains(unused));
        CHECK(changed.gone.isEmpty() && changed.replaced.isEmpty());
        CHECK(!x.mediaPool()->item(vid)->missing && !x.mediaPool()->item(unused)->missing);
        CHECK(!x.timeline()->clipAt(0).mediaMissing);
        CHECK(!x.timeline()->trackById(at)->clips.front().mediaMissing);
        CHECK(!x.modified() && x.canUndo() == couldUndo);

        // A broken copy repaired in place: never missing, but not the file
        // that was read before. Reported once, and not as an edit.
        poolSignals = timelineSignals = 0;
        {
            QFile f(vid);
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("the whole recording");
        }
        changed = x.recheckMissingMedia();
        CHECK(changed.replaced == QStringList{vid});
        CHECK(changed.gone.isEmpty() && changed.back.isEmpty());
        CHECK(poolSignals == 0 && timelineSignals == 0);
        CHECK(!x.modified() && x.canUndo() == couldUndo);
        CHECK(x.recheckMissingMedia().isEmpty());
    }
}

// A clip in two files (a ONE X2 take): the other file is saved with the
// media item, relative like its source, and comes along in a copy; the clip
// is missing, back or replaced when that file is.
void testTakeFiles()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const auto write = [](const QString &path, const char *text) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(text);
    };
    QDir().mkpath(dir + "/card");
    const QString lead = dir + "/card/VID_20250816_103807_00_001.insv";
    const QString second = dir + "/card/VID_20250816_103807_10_001.insv";
    write(lead, "lens 0");
    write(second, "lens 1");

    Project p;
    MediaItem take{lead, MediaItem::Kind::Clip360, 20000, 2880, 2880, 25.0};
    take.companions = {second};
    CHECK(p.mediaPool()->add(take));
    p.timeline()->appendClip(lead, 20000, 25.0, 2880, 2880);
    CHECK(p.mediaPool()->count() == 1 && p.mediaPool()->item(lead)->companions == QStringList{second});
    const QString orp = dir + "/ride.orp";
    CHECK(p.save(orp));
    {
        QFile f(orp);
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll().contains("\"files\":[\"card/VID_20250816_103807_10_001.insv\"]"));
    }

    // Loaded: the other file is back, absolute; with it gone, the clip is
    // missing from the start.
    Project q;
    CHECK(q.load(orp));
    CHECK(q.mediaPool()->item(lead) && q.mediaPool()->item(lead)->companions == QStringList{second});
    CHECK(!q.mediaPool()->item(lead)->missing && !q.timeline()->clipAt(0).mediaMissing);
    CHECK(QFile::rename(second, second + ".away"));
    Project r;
    CHECK(r.load(orp));
    CHECK(r.mediaPool()->item(lead)->missing && r.timeline()->clipAt(0).mediaMissing);
    CHECK(media::firstMissingFile(*r.mediaPool()->item(lead)) == second);

    // Open meanwhile: the check reports the take by its clip file.
    CHECK(q.recheckMissingMedia().gone == QStringList{lead});
    CHECK(q.mediaPool()->item(lead)->missing && q.timeline()->clipAt(0).mediaMissing);
    CHECK(QFile::rename(second + ".away", second));
    Project::MediaRecheck back = q.recheckMissingMedia();
    CHECK(back.back == QStringList{lead} && back.gone.isEmpty());
    CHECK(!q.timeline()->clipAt(0).mediaMissing);
    write(second, "lens 1, copied again");
    Project::MediaRecheck replaced = q.recheckMissingMedia();
    CHECK(replaced.replaced == QStringList{lead} && replaced.gone.isEmpty());

    // A copy of the project elsewhere keeps finding both files.
    QDir().mkpath(dir + "/elsewhere");
    const QString copy = dir + "/elsewhere/ride.orp";
    CHECK(Project::copyFile(orp, copy));
    Project c;
    CHECK(c.load(copy));
    CHECK(c.mediaPool()->item(lead) && c.mediaPool()->item(lead)->companions == QStringList{second});

    // Relinked to the same clip file: its other files are taken afresh.
    MediaItem moved = take;
    moved.companions = {dir + "/card/other.insv"};
    CHECK(q.mediaPool()->relink(lead, moved));
    CHECK(q.mediaPool()->item(lead)->companions == moved.companions);
    CHECK(q.mediaPool()->item(lead)->missing); // that one isn't there
}

// A recording the camera split over files: its parts are saved with the
// media item (version 22), relative like its source. A part that's gone
// leaves the clip playing the rest, with a gap where it was -- at load, and
// when the files are checked -- and is reported by its own file.
void testRecordingParts()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const auto write = [](const QString &path, const char *text) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(text);
    };
    QDir().mkpath(dir + "/card");
    const QString p1 = dir + "/card/VID_20250927_144221_00_019.insv";
    const QString p2 = dir + "/card/VID_20250927_144221_00_020.insv";
    write(p1, "part 1");
    write(p2, "part 2");

    Project p;
    MediaItem rec{p1, MediaItem::Kind::Clip360, 2245000, 3840, 3840, 29.97};
    rec.parts = {{p1, 0, 1799798}, {p2, 1799798, 445500}};
    CHECK(p.mediaPool()->add(rec));
    p.timeline()->appendClip(p1, 2245000, 29.97, 3840, 3840);
    CHECK(p.timeline()->clipAt(0).sourceGaps.isEmpty());
    const QString orp = dir + "/ride.orp";
    CHECK(p.save(orp));
    {
        QFile f(orp);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(text.contains("\"version\":22"));
        CHECK(text.contains("{\"durationMs\":445500,\"source\":\"card/VID_20250927_144221_00_020.insv\","
                            "\"startMs\":1799798}"));
    }

    Project q;
    CHECK(q.load(orp));
    CHECK(q.mediaPool()->item(p1) && q.mediaPool()->item(p1)->parts == rec.parts);
    CHECK(q.timeline()->clipAt(0).sourceGaps.isEmpty());

    // Part 2 gone at load: the clip isn't missing, part 2's span is a gap.
    CHECK(QFile::rename(p2, p2 + ".away"));
    Project r;
    CHECK(r.load(orp));
    const SourceGap gone{1799798, 2245298, p2};
    CHECK(!r.mediaPool()->item(p1)->missing && !r.timeline()->clipAt(0).mediaMissing);
    CHECK(r.timeline()->clipAt(0).sourceGaps == QList<SourceGap>{gone});

    // Open meanwhile: reported by the part's own file; the clip, read with
    // the parts of then, is stale.
    Project::MediaRecheck check = q.recheckMissingMedia();
    CHECK(check.gone == QStringList{p2} && check.replaced == QStringList{p1});
    CHECK(!q.timeline()->clipAt(0).mediaMissing &&
          q.timeline()->clipAt(0).sourceGaps == QList<SourceGap>{gone});
    CHECK(QFile::rename(p2 + ".away", p2));
    check = q.recheckMissingMedia();
    CHECK(check.back == QStringList{p2} && check.gone.isEmpty() &&
          check.replaced == QStringList{p1});
    CHECK(q.timeline()->clipAt(0).sourceGaps.isEmpty());

    // A part that wasn't there when the clip went in: a gap known by no file.
    const QString p3 = dir + "/card/VID_20250927_144221_00_021.insv";
    write(p3, "part 3");
    MediaItem gap{p3, MediaItem::Kind::Clip360, 0, 3840, 3840, 29.97};
    gap.path = dir + "/card/VID_20250927_144221_00_019b.insv";
    write(gap.path, "part 1 of another");
    gap.parts = {{gap.path, 0, 1000}, {p3, 3000, 500}};
    CHECK(q.mediaPool()->add(gap));
    q.timeline()->appendClip(gap.path, 3500, 29.97, 3840, 3840);
    CHECK(q.timeline()->clipAt(1).sourceGaps == (QList<SourceGap>{{1000, 3000, QString()}}));

    // An older project's item: no parts, and none read from a single entry.
    Project s;
    MediaItem one{p1, MediaItem::Kind::Clip360, 1799798, 3840, 3840, 29.97};
    one.parts = {{p1, 0, 1799798}};
    CHECK(s.mediaPool()->add(one));
    s.timeline()->appendClip(p1, 1799798, 29.97, 3840, 3840);
    const QString single = dir + "/single.orp";
    CHECK(s.save(single));
    {
        QFile f(single);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(!text.contains("\"parts\"") && !text.contains("\"version\":22"));
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

// Autosave's recovery file: written while there are unsaved changes, gone
// once they're saved or discarded, recovered as a modified project, carried
// along by a rename; an older one than the project file is stale.
void testAutosave()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    auto setTime = [](const QString &path, const QDateTime &t) {
        QFile f(path);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(t, QFileDevice::FileModificationTime);
    };
    const QString file = projectfolder::create(tmp.path(), "Ride");
    const QString recovery = Autosave::pathFor(file);
    CHECK(recovery == file + ".autosave");

    Project p;
    p.timeline()->appendClip(tmp.path() + "/a.insv", 10000, 30.0);
    CHECK(p.save(file));
    setTime(file, QDateTime::currentDateTime().addSecs(-60));
    Autosave autosave(&p, 3600 * 1000);
    autosave.tick();
    CHECK(!QFileInfo::exists(recovery)); // nothing unsaved

    p.timeline()->appendClip(tmp.path() + "/b.insv", 10000, 30.0);
    CHECK(p.modified());
    autosave.tick();
    CHECK(QFileInfo::exists(recovery));
    CHECK(Autosave::hasRecovery(file));
    {
        Project saved;
        CHECK(saved.load(file) && saved.timeline()->count() == 1); // the file is untouched
        Project q;
        CHECK(q.recover(recovery, file));
        CHECK(q.filePath() == file && q.modified());
        CHECK(q.timeline()->count() == 2);
        if (q.timeline()->count() == 2)
            CHECK(q.timeline()->clipAt(1).sourcePath == tmp.path() + "/b.insv");
    }

    // Saved: gone.
    CHECK(p.save(file));
    CHECK(!QFileInfo::exists(recovery) && !Autosave::hasRecovery(file));

    // Discarded: gone.
    p.timeline()->appendClip(tmp.path() + "/c.insv", 10000, 30.0);
    autosave.tick();
    CHECK(QFileInfo::exists(recovery));
    autosave.discard();
    CHECK(!QFileInfo::exists(recovery));

    // Another project: the old one's goes.
    autosave.tick();
    CHECK(QFileInfo::exists(recovery));
    const QString other = projectfolder::create(tmp.path(), "Other");
    CHECK(p.save(other));
    CHECK(!QFileInfo::exists(recovery));

    // Stale: older than the project file.
    {
        QFile f(recovery);
        CHECK(f.open(QIODevice::WriteOnly));
    }
    setTime(recovery, QDateTime::currentDateTime().addSecs(-3600));
    CHECK(!Autosave::hasRecovery(file));

    // A rename takes it along, folder and loose.
    setTime(recovery, QDateTime::currentDateTime().addSecs(3600));
    const QString renamed = projectfolder::rename(file, "Trip");
    CHECK(!renamed.isEmpty() && QFileInfo::exists(Autosave::pathFor(renamed)));
    CHECK(Autosave::hasRecovery(renamed));
    const QString loose = tmp.path() + "/Loose.orp";
    CHECK(p.save(loose));
    {
        QFile f(Autosave::pathFor(loose));
        CHECK(f.open(QIODevice::WriteOnly));
    }
    const QString free = projectfolder::rename(loose, "Free");
    CHECK(QFileInfo::exists(Autosave::pathFor(free)) &&
          !QFileInfo::exists(Autosave::pathFor(loose)));
}

// A measured sync / seam fine-tune isn't an unsaved change: it goes straight
// into the project file, leaving the file's other contents (and unsaved
// edits) alone, and keeps a recovery file newer than the project file.
void testMeasurementsWritten()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString file = projectfolder::create(tmp.path(), "Ride");
    const QString a = tmp.path() + "/a.insv", b = tmp.path() + "/b.insv";
    auto savedClip = [&](int i) {
        Project q;
        CHECK(q.load(file));
        return i < q.timeline()->count() ? q.timeline()->clipAt(i) : ClipItem{};
    };

    Project p;
    p.timeline()->appendClip(a, 10000, 30.0);
    p.timeline()->appendClip(b, 10000, 30.0);
    p.timeline()->appendClip(a, 10000, 30.0);
    p.timeline()->setSourceSyncAdjust(a, -1.5); // untitled: waits for the save
    CHECK(p.save(file));
    CHECK(savedClip(0).syncAdjustMs == -1.5);

    p.timeline()->setSourceSyncAdjust(a, -3.1);
    p.timeline()->setSourceSeamCorrection(b, {{-0.5, 0.25, 0.1}}, 2);
    CHECK(!p.modified()); // not an edit
    insv::SeamCorrection seam{{1.0, 1.0, 1.0}};
    CHECK(p.timeline()->sourceSyncAdjust(a) == -3.1);
    CHECK(std::isnan(p.timeline()->sourceSyncAdjust(b)));
    CHECK(std::isnan(p.timeline()->sourceSyncAdjust(tmp.path() + "/c.insv")));
    CHECK(p.timeline()->sourceSeamCorrection(b, 2, seam) && seam.lens1RotDeg[1] == 0.25);
    CHECK(!p.timeline()->sourceSeamCorrection(b, 1, seam) && seam.lens1RotDeg[1] == 0.0); // another guard
    CHECK(!p.timeline()->sourceSeamCorrection(a, 2, seam));
    CHECK(savedClip(0).syncAdjustMs == -3.1 && savedClip(2).syncAdjustMs == -3.1);
    CHECK(std::isnan(savedClip(1).syncAdjustMs));
    CHECK(savedClip(1).seamAdjustGuard == 2 && savedClip(1).seamCorrection.lens1RotDeg[1] == 0.25);

    // A fine-tune under the older key was measured against the older timing:
    // left out on load (measured again), and taken out when the new one is
    // written.
    auto fileText = [&] {
        QFile f(file);
        CHECK(f.open(QIODevice::ReadOnly));
        return f.readAll();
    };
    {
        QByteArray text = fileText();
        CHECK(text.contains("\"syncFineTuneMs\"") && !text.contains("\"syncAdjustMs\""));
        text.replace("\"syncFineTuneMs\"", "\"syncAdjustMs\"");
        QFile f(file);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
    }
    CHECK(std::isnan(savedClip(0).syncAdjustMs) && std::isnan(savedClip(2).syncAdjustMs));
    p.timeline()->setSourceSyncAdjust(a, 0.4);
    CHECK(savedClip(0).syncAdjustMs == 0.4 && !fileText().contains("\"syncAdjustMs\""));

    // The lens colour likewise: per source and lens guard, straight into the file.
    insv::LensColour colour;
    colour.logRatio[2] = -0.06;
    colour.slope[0] = 0.004;
    colour.spread = 0.2;
    p.timeline()->setSourceLensColour(b, colour, 2);
    CHECK(!p.modified());
    insv::LensColour got;
    CHECK(p.timeline()->sourceLensColour(b, 2, got) && got == colour);
    CHECK(!p.timeline()->sourceLensColour(b, 1, got) && got == insv::LensColour{});
    CHECK(!p.timeline()->sourceLensColour(a, 2, got));
    CHECK(savedClip(1).lensColourGuard == 2 && savedClip(1).lensColour == colour);
    CHECK(savedClip(0).lensColourGuard == -1);

    // With unsaved edits: only the measurement reaches the file.
    p.timeline()->appendClip(b, 10000, 30.0);
    CHECK(p.modified());
    Autosave autosave(&p, 3600 * 1000);
    autosave.tick();
    {
        QFile f(Autosave::pathFor(file)); // make it older than the file will be
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(QDateTime::currentDateTime().addSecs(-60),
                          QFileDevice::FileModificationTime);
    }
    p.timeline()->setSourceSyncAdjust(b, 2.0);
    CHECK(p.modified());
    {
        Project q;
        CHECK(q.load(file) && q.timeline()->count() == 3);
        if (q.timeline()->count() == 3)
            CHECK(q.timeline()->clipAt(1).syncAdjustMs == 2.0);
    }
    CHECK(Autosave::hasRecovery(file)); // rewritten after the file
}

// A seam fine-tune measured by the older estimator (lens-1 rotation only, no
// "method") isn't read back: the clip is measured again.
void testOldSeamMeasurement()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString file = projectfolder::create(tmp.path(), "Ride");
    const QString a = tmp.path() + "/a.insv";
    {
        Project p;
        p.timeline()->appendClip(a, 10000, 30.0);
        p.timeline()->setSourceSeamCorrection(a, {{-0.5, 0.25, 0.1}, {0.001, 0.0}, 1.001}, 2);
        CHECK(p.save(file));
    }
    {
        Project q;
        CHECK(q.load(file) && q.timeline()->count() == 1);
        insv::SeamCorrection seam;
        CHECK(q.timeline()->sourceSeamCorrection(a, 2, seam) && seam.radialScale == 1.001);
    }
    // Rewritten as the older estimator saved it.
    QFile f(file);
    CHECK(f.open(QIODevice::ReadOnly));
    QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    QJsonObject timeline = doc["timeline"].toObject();
    QJsonArray clips = timeline["clips"].toArray();
    QJsonObject clip = clips[0].toObject();
    clip["seamAdjust"] = QJsonObject{{"deg", QJsonArray{-0.5, 0.25, 0.1}}, {"lensGuard", 2}};
    clips[0] = clip;
    timeline["clips"] = clips;
    doc["timeline"] = timeline;
    CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(doc).toJson());
    f.close();
    Project q;
    CHECK(q.load(file) && q.timeline()->count() == 1);
    insv::SeamCorrection seam;
    CHECK(!q.timeline()->sourceSeamCorrection(a, 2, seam) && seam == insv::SeamCorrection{});
    CHECK(q.timeline()->clipAt(0).seamAdjustGuard == -1);
}

// A lens colour from another estimator (a "method" this build doesn't know)
// isn't read back: the clip is measured again. One from this one is.
void testLensColourMethod()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString file = projectfolder::create(tmp.path(), "Ride");
    const QString a = tmp.path() + "/a.insv";
    insv::LensColour colour;
    colour.logRatio[0] = 0.03;
    colour.slope[1] = -0.002;
    {
        Project p;
        p.timeline()->appendClip(a, 10000, 30.0);
        p.timeline()->setSourceLensColour(a, colour, 2);
        CHECK(p.save(file));
    }
    {
        Project q;
        CHECK(q.load(file) && q.timeline()->count() == 1);
        insv::LensColour got;
        CHECK(q.timeline()->sourceLensColour(a, 2, got) && got == colour);
    }
    QFile f(file);
    CHECK(f.open(QIODevice::ReadOnly));
    QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    QJsonObject timeline = doc["timeline"].toObject();
    QJsonArray clips = timeline["clips"].toArray();
    QJsonObject clip = clips[0].toObject();
    QJsonObject lc = clip["lensColour"].toObject();
    CHECK(lc["method"].toInt() == insv::kLensColourMethod);
    lc["method"] = insv::kLensColourMethod + 1;
    clip["lensColour"] = lc;
    clips[0] = clip;
    timeline["clips"] = clips;
    doc["timeline"] = timeline;
    CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(doc).toJson());
    f.close();
    Project q;
    CHECK(q.load(file) && q.timeline()->count() == 1);
    CHECK(q.timeline()->clipAt(0).lensColourGuard == -1);
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

} // namespace

// A flat video on the main track (ClipItem::flat): its fit and edits round
// trip, the 360 fields aren't written, and the file is version 11 only while
// it has one -- a project without stays readable by the builds before.
// The app's kept calibrations: one per camera and guard, found again for a
// clip whose file carries the calibration it was made against, and only then.
void testLensCalibrationStore()
{
    QStandardPaths::setTestModeEnabled(true); // not the user's data folder
    stitch::LensCalibration cal;
    cal.serial = "IAHEA25088AYDQ";
    cal.lensGuard = 0;
    cal.created = "2026-11-02T10:00:00Z";
    cal.squareMm = 41.96;
    for (int i = 0; i < 2; ++i) {
        insv::LensCalibExt e;
        e.xi = 2, e.fx = 4294.03 - 16 * i, e.fy = 4293.86, e.cx = 2703.31 + 5370 * i;
        e.cy = 2684.03, e.w = 10752, e.h = 5376, e.focalRefW = 5312;
        cal.fileLenses[i] = e;
        e.fx -= 23.7;
        cal.lenses[i] = e;
    }
    QString err;
    CHECK(lenscal::Save(cal, &err));
    CHECK(QFile::exists(lenscal::StoreDir() + "/IAHEA25088AYDQ-none.json"));
    CHECK(lenscal::Load("IAHEA25088AYDQ", 0) == cal);
    CHECK(!lenscal::Load("IAHEA25088AYDQ", 2)); // another guard
    CHECK(!lenscal::Load("OTHER", 0));
    CHECK(lenscal::ForClip("IAHEA25088AYDQ", 0, cal.fileLenses[0], cal.fileLenses[1]) == cal);
    insv::LensCalibExt other = cal.fileLenses[1];
    other.fx += 1; // a file with another calibration: not for it
    CHECK(!lenscal::ForClip("IAHEA25088AYDQ", 0, cal.fileLenses[0], other));
    QDir(lenscal::StoreDir()).removeRecursively();
    QStandardPaths::setTestModeEnabled(false);
}

// A clip's own lens calibration: kept on every clip of its file, saved per
// clip in full (the project stitches the same without the app's copy),
// undoable; setting it drops the seam measured through the old one; the
// version goes up only while a clip has one.
void testLensCalibrationJson()
{
    using G = stitch::PreviewStitchConfig::LensGuard;
    QTemporaryDir tmp;
    const QString a = tmp.path() + "/a.insv";
    writeFile(a);
    const QString file = tmp.path() + "/cal.orp";
    auto document = [&file] {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };
    stitch::LensCalibration cal;
    cal.serial = "IAHEA25088AYDQ";
    cal.lensGuard = 2;
    cal.created = "2026-11-02T10:00:00Z";
    cal.squareMm = 41.96;
    cal.rmsPx[0] = 0.22, cal.rmsPx[1] = 0.21;
    cal.transferDeg = 0.019;
    cal.coverageDeg[0] = 100, cal.coverageDeg[1] = 95;
    for (int i = 0; i < 2; ++i) {
        insv::LensCalibExt e;
        e.xi = 2, e.fx = 4294.03 + i, e.fy = 4293.86, e.cx = 2703.31 + 5370 * i, e.cy = 2684.03;
        e.yaw = 0.241 / 3, e.pitch = 0.068, e.roll = 89.993, e.tz = -0.031796 * i;
        e.k[0] = 0.22969893, e.k[3] = -3.16975212, e.p[1] = 0.00040147, e.s[2] = 0.00195414;
        e.w = 10752, e.h = 5376, e.focalRefW = 5312, e.type = 113;
        cal.fileLenses[i] = e;
        e.fx *= 0.9947, e.yaw += 0.1234567890123;
        cal.lenses[i] = e;
    }

    QString err;
    {
        Project p;
        Timeline *tl = p.timeline();
        const int a1 = tl->appendClip(a, 10000, 30.0, 7680, 3840, 2);
        tl->setSourceSeamCorrection(a, {{-0.5, 0.25, 0.1}}, 2);
        CHECK(!tl->clipById(a1)->lensCalibration);
        tl->setSourceStitch(a, G::Premium, stitch::PreviewStitchConfig::Method::DepthSeam, cal);
        const int a2 = tl->appendClip(a, 10000, 30.0, 7680, 3840, 2); // the same file
        CHECK(tl->clipById(a1)->lensCalibration == cal && tl->clipById(a2)->lensCalibration == cal);
        // Measured through the file's calibration: gone.
        insv::SeamCorrection seam;
        CHECK(!tl->sourceSeamCorrection(a, seam) && tl->clipById(a1)->seamAdjustGuard == -1);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kStitchFormatVersion);
        const QJsonObject st = document()
                                   .value("timeline").toObject()
                                   .value("clips").toArray().at(0).toObject()
                                   .value("stitch").toObject();
        CHECK(st.value("calibration").toObject().value("serial").toString() == "IAHEA25088AYDQ");
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        Timeline *tl = p.timeline();
        CHECK(tl->count() == 2);
        // Read back to the last bit: it stitches the same.
        CHECK(tl->clipAt(0).lensCalibration == cal && tl->clipAt(1).lensCalibration == cal);
        tl->setSourceStitch(a, G::Premium, stitch::PreviewStitchConfig::Method::DepthSeam, std::nullopt);
        CHECK(!tl->clipAt(0).lensCalibration && !tl->clipAt(1).lensCalibration);
        p.undo();
        CHECK(tl->clipAt(0).lensCalibration == cal);
        tl->setSourceStitch(a, G::Premium, stitch::PreviewStitchConfig::Method::DepthSeam, std::nullopt);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() < Project::kStitchFormatVersion);
    }
}

// Stitch settings are per source file: kept on every clip of it, saved per
// clip, and an older project's one setting becomes every clip's.
// Highlight marks: the user's own and the camera's hidden ones, per clip,
// written only when a clip has one -- as version 21, which the builds
// before refuse rather than drop them on save; a flat clip's own ones too.
void testMarksJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString a = tmp.path() + "/a.insv";
    const QString v = tmp.path() + "/phone.mp4";
    writeFile(a);
    writeFile(v);
    const QString file = tmp.path() + "/marks.orp";
    auto document = [&file] {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };

    QString err;
    {
        Project p;
        Timeline *tl = p.timeline();
        const int c = tl->appendClip(a, 10000, 30.0, 7680, 3840, 2);
        const int f = tl->appendClip(v, 5000, 30.0, 1920, 1080, 0, true);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() < Project::kMarkFormatVersion);
        QJsonObject jc = document().value("timeline").toObject().value("clips").toArray().at(0).toObject();
        CHECK(!jc.contains("marks") && !jc.contains("hiddenMarks"));

        tl->addClipMark(c, 6000);
        tl->addClipMark(c, 2000);
        tl->setClipMarkHidden(c, 4000, true); // a camera mark
        tl->addClipMark(f, 1500);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kMarkFormatVersion);
        const QJsonArray clips = document().value("timeline").toObject().value("clips").toArray();
        jc = clips.at(0).toObject();
        CHECK(jc.value("marks").toArray() == QJsonArray({2000, 6000}));
        CHECK(jc.value("hiddenMarks").toArray() == QJsonArray({4000}));
        CHECK(clips.at(1).toObject().value("marks").toArray() == QJsonArray({1500}));
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        Timeline *tl = p.timeline();
        CHECK(tl->clipAt(0).marks == (QList<qint64>{2000, 6000}));
        CHECK(tl->clipAt(0).hiddenMarks == QList<qint64>{4000});
        CHECK(tl->clipAt(1).marks == QList<qint64>{1500});
        CHECK(!p.modified());

        // An edit, and undo puts it back.
        tl->removeClipMark(tl->clipAt(0).id, 2000);
        CHECK(p.canUndo() && p.modified());
        p.undo();
        CHECK(tl->clipAt(0).marks == (QList<qint64>{2000, 6000}));
        p.redo();
        CHECK(tl->clipAt(0).marks == QList<qint64>{6000});

        // None left: the version before again.
        tl->removeClipMark(tl->clipAt(0).id, 6000);
        tl->showClipHiddenMarks(tl->clipAt(0).id);
        tl->removeClipMark(tl->clipAt(1).id, 1500);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() < Project::kMarkFormatVersion);
    }
    {
        // A version 20 file: no marks; one with a version past this
        // build's is refused.
        writeFile(file, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":20,"timeline":{"clips":[{"source":"a.insv","sourceDurationMs":10000,"fps":30}]}})"));
        Project p;
        CHECK(p.load(file, &err));
        CHECK(p.timeline()->count() == 1 && p.timeline()->clipAt(0).marks.isEmpty() &&
              p.timeline()->clipAt(0).hiddenMarks.isEmpty());
        writeFile(file, QByteArrayLiteral(
                            R"({"format":"orbis-reframe-project","version":23,"timeline":{"clips":[]}})"));
        CHECK(!p.load(file, &err));
    }
}

// A media pool file's highlight marks (set in the Media tab's preview):
// edited as a clip's are, over the whole file; saved in the project as
// version 22, undone without touching the timeline, carried by a relink.
void testMediaMarks()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString a = tmp.path() + "/a.insv";
    const QString moved = tmp.path() + "/moved.insv";
    const QString v = tmp.path() + "/phone.mp4";
    const QString w = tmp.path() + "/song.wav";
    for (const QString &f : {a, moved, v, w})
        writeFile(f);
    const QString file = tmp.path() + "/pool.orp";
    auto document = [&file] {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };
    const QList<qint64> camera{4000, 8000};

    Project p;
    MediaPool *pool = p.mediaPool();
    CHECK(pool->add({a, MediaItem::Kind::Clip360, 10000, 3840, 1920, 30.0}));
    CHECK(pool->add({v, MediaItem::Kind::Video, 5000, 1920, 1080, 30.0}));
    CHECK(pool->add({w, MediaItem::Kind::Audio, 5000}));
    const auto item = [&](const QString &path) { return *pool->item(path); };

    pool->addMark(a, 2000, camera);
    pool->addMark(a, 2010, camera); // within a frame of it
    pool->addMark(a, 20000, camera); // clamped to the end
    pool->removeMarkNear(a, 4100, camera); // the camera's: hidden
    CHECK(item(a).marks == (QList<qint64>{2000, 10000}));
    CHECK(item(a).hiddenMarks == QList<qint64>{4000});
    CHECK(item(a).shownMarks(camera) ==
          (QList<highlight::Mark>{{2000, true}, {8000, false}, {10000, true}}));
    pool->addMark(a, 4000, camera); // shows the hidden one again
    CHECK(item(a).hiddenMarks.isEmpty() && item(a).marks.size() == 2);
    pool->setMarkHidden(a, 8000, true);
    pool->addMark(v, 1500, camera); // a flat video: its own only
    pool->setMarkHidden(v, 4000, true);
    CHECK(item(v).marks == QList<qint64>{1500} && item(v).hiddenMarks.isEmpty());
    CHECK(item(v).shownMarks(camera).size() == 1);
    pool->addMark(w, 1000, {}); // an audio file: none
    CHECK(item(w).marks.isEmpty());

    QString err;
    CHECK(p.save(file, &err));
    CHECK(document().value("version").toInt() == Project::kPartsFormatVersion);
    const QJsonArray media = document().value("media").toArray();
    CHECK(media.at(0).toObject().value("marks").toArray() == QJsonArray({2000, 10000}));
    CHECK(media.at(0).toObject().value("hiddenMarks").toArray() == QJsonArray({8000}));
    CHECK(media.at(1).toObject().value("marks").toArray() == QJsonArray({1500}));
    CHECK(!media.at(2).toObject().contains("marks"));

    Project q;
    CHECK(q.load(file, &err));
    MediaPool *qpool = q.mediaPool();
    CHECK(qpool->item(a)->marks == (QList<qint64>{2000, 10000}));
    CHECK(qpool->item(a)->hiddenMarks == QList<qint64>{8000});
    CHECK(qpool->item(v)->marks == QList<qint64>{1500});
    CHECK(!q.modified());

    // An edit to the pool is one undo step, and the timeline isn't in it.
    q.timeline()->appendClip(a, 10000, 30.0, 3840, 1920);
    q.undo();
    CHECK(!q.lastStepOnlyMedia() && q.timeline()->count() == 0);
    q.redo();
    qpool->removeMark(a, 2000);
    qpool->showHiddenMarks(a);
    CHECK(q.canUndo());
    q.undo();
    CHECK(q.lastStepOnlyMedia());
    CHECK(qpool->item(a)->marks == (QList<qint64>{2000, 10000}));
    CHECK(qpool->item(a)->hiddenMarks == QList<qint64>{8000});
    CHECK(q.timeline()->count() == 1);

    // A relink carries them; into an item already there, with its own.
    MediaItem to{moved, MediaItem::Kind::Clip360, 10000, 3840, 1920, 30.0};
    CHECK(q.relinkMedia(a, to) >= 0);
    CHECK(qpool->item(moved) && qpool->item(moved)->marks == (QList<qint64>{2000, 10000}));
    CHECK(qpool->add({a, MediaItem::Kind::Clip360, 10000, 3840, 1920, 30.0}));
    qpool->addMark(a, 3000, {});
    CHECK(q.relinkMedia(a, to) >= 0);
    CHECK(!qpool->contains(a));
    CHECK(qpool->item(moved)->marks == (QList<qint64>{2000, 3000, 10000}));

    // No marks left: the version before (no parts either).
    q.timeline()->removeClip(q.timeline()->clipAt(0).id);
    qpool->setItems({});
    CHECK(q.save(file, &err));
    CHECK(document().value("version").toInt() < Project::kPartsFormatVersion);
}

void testClipStitchJson()
{
    using G = stitch::PreviewStitchConfig::LensGuard;
    using M = stitch::PreviewStitchConfig::Method;
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString a = tmp.path() + "/a.insv";
    const QString b = tmp.path() + "/b.insv";
    writeFile(a);
    writeFile(b);
    const QString file = tmp.path() + "/stitch.orp";
    auto document = [&file] {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };

    QString err;
    {
        Project p;
        Timeline *tl = p.timeline();
        const int a1 = tl->appendClip(a, 10000, 30.0, 7680, 3840, 2);
        // A clip of a project with no 360 clip yet: the new stitcher.
        CHECK(tl->clipById(a1)->lensGuard == G::Premium && tl->clipById(a1)->stitchMethod == M::New);
        tl->setSourceStitch(a, G::None, M::FixedSeam, std::nullopt);
        const int a2 = tl->appendClip(a, 10000, 30.0, 7680, 3840, 2); // the same file
        CHECK(tl->clipById(a2)->lensGuard == G::None && tl->clipById(a2)->stitchMethod == M::FixedSeam);
        const int b1 = tl->appendClip(b, 8000, 30.0, 7680, 3840, 2); // the neighbour's
        CHECK(tl->clipById(b1)->lensGuard == G::None && tl->clipById(b1)->stitchMethod == M::FixedSeam);
        tl->setSourceStitch(b, G::Standard, M::DepthSeam, std::nullopt);
        CHECK(tl->clipById(a1)->lensGuard == G::None && tl->clipById(b1)->lensGuard == G::Standard);
        CHECK(p.save(file, &err));
        // Mixed: the builds before, which read one setting for all, refuse it.
        CHECK(document().value("version").toInt() == Project::kFlatClipFormatVersion);
        const QJsonArray clips = document().value("timeline").toObject().value("clips").toArray();
        CHECK(clips.at(0).toObject().value("stitch").toObject().value("lensGuard").toString() ==
              "none");
        CHECK(clips.at(2).toObject().value("stitch").toObject().value("lensGuard").toString() ==
              "standard");

    }
    {
        Project p;
        CHECK(p.load(file, &err));
        const Timeline *tl = p.timeline();
        CHECK(tl->count() == 3);
        CHECK(tl->clipAt(0).lensGuard == G::None && tl->clipAt(0).stitchMethod == M::FixedSeam);
        CHECK(tl->clipAt(1).lensGuard == G::None);
        CHECK(tl->clipAt(2).lensGuard == G::Standard && tl->clipAt(2).stitchMethod == M::DepthSeam);

        // An edit, and undo puts it back.
        p.timeline()->setSourceStitch(b, G::Premium, M::FixedSeam, std::nullopt);
        CHECK(p.canUndo() && p.modified());
        p.undo();
        CHECK(tl->clipAt(2).lensGuard == G::Standard && tl->clipAt(2).stitchMethod == M::DepthSeam);

        // All the same: still the plain version, the project-wide value theirs.
        p.timeline()->setSourceStitch(b, G::None, M::FixedSeam, std::nullopt);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kPlainFormatVersion);
        const QJsonObject ps = document().value("previewStitch").toObject();
        CHECK(ps.value("lensGuard").toString() == "none" && !ps.value("depthSeam").toBool(true));
    }
    {
        // The new stitcher: written as "method": "new" with its seam colour
        // blend and seam flow (on by default).
        Project p;
        CHECK(p.load(file, &err));
        CHECK(p.timeline()->clipAt(2).seamColourBlend && p.timeline()->clipAt(2).seamFlow);
        p.timeline()->setSourceStitch(b, G::None, M::New, std::nullopt);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kStitchFormatVersion);
        QJsonArray clips = document().value("timeline").toObject().value("clips").toArray();
        QJsonObject st = clips.at(2).toObject().value("stitch").toObject();
        CHECK(st.value("method").toString() == "new" && !st.value("depthSeam").toBool(true));
        CHECK(st.value("seamColourBlend").toBool(false) && st.value("seamFlow").toBool(false));
        CHECK(!clips.at(0).toObject().value("stitch").toObject().contains("method"));
        CHECK(!clips.at(0).toObject().value("stitch").toObject().contains("seamColourBlend"));
        CHECK(!clips.at(0).toObject().value("stitch").toObject().contains("seamFlow"));
        p.timeline()->setSourceSeamColourBlend(b, false);
        p.timeline()->setSourceSeamFlow(b, false);
        CHECK(p.save(file, &err));
        clips = document().value("timeline").toObject().value("clips").toArray();
        st = clips.at(2).toObject().value("stitch").toObject();
        CHECK(st.contains("seamColourBlend") && !st.value("seamColourBlend").toBool(true));
        CHECK(st.contains("seamFlow") && !st.value("seamFlow").toBool(true));
        Project q;
        CHECK(q.load(file, &err));
        CHECK(q.timeline()->clipAt(2).stitchMethod == M::New);
        CHECK(!q.timeline()->clipAt(2).seamColourBlend && !q.timeline()->clipAt(2).seamFlow);
        q.timeline()->setSourceSeamFlow(b, true);
        CHECK(q.canUndo() && q.modified() && q.timeline()->clipAt(2).seamFlow);
        q.undo();
        CHECK(!q.timeline()->clipAt(2).seamFlow);
        CHECK(q.timeline()->clipAt(0).stitchMethod == M::FixedSeam);
        q.timeline()->setSourceSeamColourBlend(b, true);
        CHECK(q.canUndo() && q.modified() && q.timeline()->clipAt(2).seamColourBlend);
        q.undo();
        CHECK(!q.timeline()->clipAt(2).seamColourBlend);
        q.timeline()->setSourceSeamColourBlend(b, true);
        CHECK(q.save(file, &err));
        Project r;
        CHECK(r.load(file, &err));
        CHECK(r.timeline()->clipAt(2).seamColourBlend);
        q.timeline()->setSourceStitch(b, G::None, M::FixedSeam, std::nullopt);
        CHECK(q.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kPlainFormatVersion);
    }
    {
        // An older project: one setting, no "stitch" on the clips.
        QJsonObject doc = document();
        QJsonObject ps = doc.value("previewStitch").toObject();
        ps["lensGuard"] = "standard";
        ps["depthSeam"] = true;
        doc["previewStitch"] = ps;
        QJsonObject timeline = doc.value("timeline").toObject();
        QJsonArray clips = timeline.value("clips").toArray();
        for (int i = 0; i < clips.size(); ++i) {
            QJsonObject c = clips.at(i).toObject();
            c.remove("stitch");
            clips[i] = c;
        }
        timeline["clips"] = clips;
        doc["timeline"] = timeline;
        QFile f(file);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(doc).toJson());
        f.close();
        Project p;
        CHECK(p.load(file, &err));
        for (int i = 0; i < p.timeline()->count(); ++i)
            CHECK(p.timeline()->clipAt(i).lensGuard == G::Standard &&
                  p.timeline()->clipAt(i).stitchMethod == M::DepthSeam);

        // A file that doesn't name a stitcher at all is from before the new
        // stitcher became the default: the old one, as it was saved with.
        ps.remove("depthSeam");
        doc["previewStitch"] = ps;
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(doc).toJson());
        f.close();
        Project q;
        CHECK(q.load(file, &err));
        for (int i = 0; i < q.timeline()->count(); ++i)
            CHECK(q.timeline()->clipAt(i).stitchMethod == M::DepthSeam);
        doc.remove("previewStitch");
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(doc).toJson());
        f.close();
        Project r;
        CHECK(r.load(file, &err));
        for (int i = 0; i < r.timeline()->count(); ++i)
            CHECK(r.timeline()->clipAt(i).stitchMethod == M::DepthSeam);
        for (int i = 0; i < clips.size(); ++i) { // a clip's own "stitch" without one
            QJsonObject c = clips.at(i).toObject();
            c["stitch"] = QJsonObject{{"lensGuard", "premium"}};
            clips[i] = c;
        }
        timeline["clips"] = clips;
        doc["timeline"] = timeline;
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(doc).toJson());
        f.close();
        Project s;
        CHECK(s.load(file, &err));
        for (int i = 0; i < s.timeline()->count(); ++i)
            CHECK(s.timeline()->clipAt(i).lensGuard == G::Premium &&
                  s.timeline()->clipAt(i).stitchMethod == M::DepthSeam);
    }
    {
        // A new project's clip: the new stitcher, saved as such and read back.
        Project p;
        p.timeline()->appendClip(a, 10000, 30.0, 7680, 3840, 2);
        CHECK(p.save(file, &err));
        const QJsonObject st = document()
                                   .value("timeline").toObject()
                                   .value("clips").toArray()
                                   .at(0).toObject()
                                   .value("stitch").toObject();
        CHECK(st.value("method").toString() == "new");
        Project q;
        CHECK(q.load(file, &err));
        CHECK(q.timeline()->clipAt(0).stitchMethod == M::New && q.timeline()->clipAt(0).seamColourBlend &&
              q.timeline()->clipAt(0).seamFlow);
    }
}

void testFlatClipJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString insv = tmp.path() + "/ride.insv";
    const QString phone = tmp.path() + "/phone.mp4";
    writeFile(insv);
    writeFile(phone);
    const QString file = tmp.path() + "/mixed.orp";
    auto document = [&file] {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };
    auto versionOf = [&document] { return document().value("version").toInt(); };
    auto clipJson = [&document](int i) {
        return document().value("timeline").toObject().value("clips").toArray().at(i).toObject();
    };

    QString err;
    {
        Project p;
        const int a = p.timeline()->appendClip(insv, 10000, 30.0, 7680, 3840, 2);
        p.timeline()->setClipStabilization(a, insv::StabConfig::preset(insv::StabMode::LeanFollow));
        const int b = p.timeline()->appendClip(phone, 8000, 60.0, 1080, 1920, 1, true);
        const ClipItem *flat = p.timeline()->clipById(b);
        CHECK(flat && flat->flat && flat->fit == ClipItem::Fit::Bars);
        // a flat clip doesn't take its neighbour's stabilization
        CHECK(flat && flat->stabilization.mode == insv::StabMode::Off);
        p.timeline()->setClipFit(b, ClipItem::Fit::Fill);
        p.timeline()->setClipTrimIn(b, 1000);
        p.timeline()->setClipSpeed(b, 2.0);
        ColorGrade g;
        g.saturation = 40.0f;
        p.timeline()->setClipColor(b, g);
        // the 360-only edits don't take on a flat clip
        p.timeline()->upsertClipReframeKey(b, 2000, {0.5f, 0.0f, 0.0f, 1.0f});
        p.timeline()->setClipBlurPlates(b, true);
        p.timeline()->setClipStabilization(b, insv::StabConfig::preset(insv::StabMode::LeanFollow));
        CHECK(flat->reframe.isEmpty() && !flat->blurPlates &&
              flat->stabilization.mode == insv::StabMode::Off);
        p.timeline()->setClipFit(a, ClipItem::Fit::Fill); // no-op on a 360 clip
        CHECK(p.timeline()->clipById(a)->fit == ClipItem::Fit::Bars);
        // a 360 clip after a flat one takes the 360 clip's stabilization
        const int c = p.timeline()->appendClip(insv, 10000, 30.0, 7680, 3840, 0);
        CHECK(p.timeline()->clipById(c)->stabilization.mode == insv::StabMode::LeanFollow);
        useOldStitcher(*p.timeline());
        CHECK(p.save(file, &err));
        CHECK(versionOf() == 11);
        const QJsonObject jb = clipJson(1);
        CHECK(jb.value("flat").toBool() && jb.value("fit").toString() == "fill");
        CHECK(!jb.contains("reframe") && !jb.contains("stabilization"));
        // the pool lists it as a video, not a 360 clip
        const MediaItem *m = p.mediaPool()->item(phone);
        CHECK(m && m->kind == MediaItem::Kind::Video);
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        CHECK(p.timeline()->count() == 3);
        const ClipItem &a = p.timeline()->clipAt(0);
        const ClipItem &b = p.timeline()->clipAt(1);
        CHECK(!a.flat && b.flat && b.fit == ClipItem::Fit::Fill);
        CHECK(b.trimInMs == 1000 && b.speed == 2.0 && b.color.saturation == 40.0f);
        CHECK(b.sourceWidth == 1080 && b.sourceHeight == 1920 && b.fps == 60.0);
        // its sound is still linked to it, on the camera mic's track
        const Track *cam =
            p.timeline()->trackById(p.timeline()->streamTrackId(Track::Role::CameraMic));
        bool linked = false;
        for (const TrackClip &tc : cam ? cam->clips : QList<TrackClip>())
            linked = linked || tc.linkedClipId == b.id;
        CHECK(linked);
        // without the flat clip it's version 10 again
        p.timeline()->removeClip(b.id);
        useOldStitcher(*p.timeline());
        CHECK(p.save(file, &err));
        CHECK(versionOf() == 10);
    }
}

// The plate scans a project's cache must keep: the clips' now, and those
// an undo or redo would bring back.
void testPlateScansInUse()
{
    Project p;
    const int a = p.timeline()->appendClip("a.insv", 10000, 30.0);
    p.timeline()->setClipPlateScan(a, "old");
    p.undo();
    p.redo(); // settled
    const int id = p.timeline()->clipAt(0).id;
    p.timeline()->setClipPlateScan(id, "new"); // rescanned
    p.timeline()->duplicateClip(id);
    CHECK(p.plateScansInUse() == (QSet<QString>{"old", "new"}));
    p.undo(); // the rescan and duplicate undone: "new" is a redo away
    CHECK(p.timeline()->clipAt(0).plateScan == "old");
    CHECK(p.plateScansInUse() == (QSet<QString>{"old", "new"}));
    p.timeline()->setClipBlurPlates(p.timeline()->clipAt(0).id, true); // drops the redo
    CHECK(p.plateScansInUse() == QSet<QString>{"old"});
}

// A title round trips with its style, keys and timing; it has no source and
// isn't in the media pool; the file is version 12 only while it has one.
void testTitleJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString video = tmp.path() + "/clip.insv";
    writeFile(video);
    const QString file = tmp.path() + "/titles.orp";
    auto document = [&file] {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };
    QString err;
    {
        Project p;
        Timeline *tl = p.timeline();
        tl->appendClip(video, 10000, 30.0, 7680, 3840, 0);
        useOldStitcher(*tl);
        const int v = tl->addTrack(Track::Kind::Video, "Text");
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kPlainFormatVersion);
        title::Style style;
        style.text = "Day 1\nAlps";
        style.font = "DejaVu Serif";
        style.bold = false;
        style.italic = true;
        style.rgb = 0x10ff80;
        const int id = tl->addTitleClip(v, 1500, style, 3000);
        tl->setTrackClipFades(id, 250, 500);
        inset::Look l = tl->trackClipById(id)->look;
        l.width = 0.12f;
        tl->upsertTrackClipInsetKey(id, tl->trackClipById(id)->trimInMs, l);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kTitleFormatVersion);
        const QJsonObject jc = document()
                                   .value("timeline").toObject()
                                   .value("tracks").toArray().at(0).toObject()
                                   .value("clips").toArray().at(0).toObject();
        CHECK(!jc.contains("source") && !jc.contains("sourceWidth"));
        CHECK(jc.value("title").toObject().value("color").toString() == "#10ff80");
        CHECK(p.mediaPool()->count() == 1); // the 360 clip only
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        const Timeline *tl = p.timeline();
        CHECK(tl->trackCount() == 1 && tl->trackAt(0).clips.size() == 1);
        const TrackClip &c = tl->trackAt(0).clips.at(0);
        CHECK(c.isTitle() && c.sourcePath.isEmpty() && !c.mediaMissing);
        CHECK(c.title->text == "Day 1\nAlps" && c.title->font == "DejaVu Serif");
        CHECK(!c.title->bold && c.title->italic && c.title->rgb == 0x10ff80);
        CHECK(c.startMs == 1500 && c.timelineDurationMs() == 3000);
        CHECK(c.fadeInMs == 250 && c.fadeOutMs == 500);
        CHECK(c.insetKeys.count() == 1 && c.insetKeys.keys().at(0).look.width == 0.12f);
        CHECK(p.mediaPool()->count() == 1);
        CHECK(c.title->align == title::Align::Centre && !c.title->outline.on &&
              !c.title->shadow.on && !c.title->box.on); // a plain title's look
    }
    // Its look: written only as far as it isn't plain, with the version that
    // added it, and read back.
    title::Style look;
    {
        Project p;
        CHECK(p.load(file, &err));
        Timeline *tl = p.timeline();
        look = *tl->trackAt(0).clips.at(0).title;
        look.align = title::Align::Right;
        look.lineSpacing = 1.25f;
        look.letterSpacing = -0.05f;
        look.outline = {true, 0.08f, 0x202020};
        look.shadow = {true, 0x000040, 0.5f, 0.1f, 0.2f};
        look.box = {true, 0xffcc00, 0.75f, 0.4f, 0.25f};
        tl->setTrackClipTitle(tl->trackAt(0).clips.at(0).id, look);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kTitleLookFormatVersion);
        const QJsonObject jt = document()
                                   .value("timeline").toObject()
                                   .value("tracks").toArray().at(0).toObject()
                                   .value("clips").toArray().at(0).toObject()
                                   .value("title").toObject();
        CHECK(jt.value("align").toString() == "right");
        CHECK(jt.value("box").toObject().value("color").toString() == "#ffcc00");
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        Timeline *tl = p.timeline();
        CHECK(*tl->trackAt(0).clips.at(0).title == look);
        // Drawn plain again: the file goes back to the version a plain title
        // needs, and keeps the parts switched off for switching on again.
        title::Style plain = look;
        plain.align = title::Align::Centre;
        plain.lineSpacing = 1.0f;
        plain.letterSpacing = 0.0f;
        plain.outline.on = plain.shadow.on = plain.box.on = false;
        tl->setTrackClipTitle(tl->trackAt(0).clips.at(0).id, plain);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kTitleFormatVersion);
        const QJsonObject jt = document()
                                   .value("timeline").toObject()
                                   .value("tracks").toArray().at(0).toObject()
                                   .value("clips").toArray().at(0).toObject()
                                   .value("title").toObject();
        for (const char *key : {"align", "lineSpacing", "letterSpacing"})
            CHECK(!jt.contains(QLatin1String(key)));
        CHECK(!jt.value("box").toObject().value("on").toBool(true));
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        const title::Style &t = *p.timeline()->trackAt(0).clips.at(0).title;
        CHECK(!t.box.on && t.box.rgb == 0xffcc00 && !t.outline.on && t.outline.width == 0.08f);
    }
    // Its motion: written only when it has one, with the version that added
    // it, and read back; none again, and the file goes back.
    inset::Motion motion;
    motion.in = {inset::Motion::Kind::Slide, inset::Motion::Side::Bottom, 750};
    motion.out = {inset::Motion::Kind::Pop, inset::Motion::Side::Left, 300};
    {
        Project p;
        CHECK(p.load(file, &err));
        Timeline *tl = p.timeline();
        tl->setTrackClipMotion(tl->trackAt(0).clips.at(0).id, motion);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kMotionFormatVersion);
        const QJsonObject jm = document()
                                   .value("timeline").toObject()
                                   .value("tracks").toArray().at(0).toObject()
                                   .value("clips").toArray().at(0).toObject()
                                   .value("motion").toObject();
        CHECK(jm.value("in").toObject().value("side").toString() == "bottom");
        CHECK(jm.value("out").toObject().value("kind").toString() == "pop" &&
              !jm.value("out").toObject().contains("side"));
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        Timeline *tl = p.timeline();
        CHECK(tl->trackAt(0).clips.at(0).motion == motion);
        // Typing on and off.
        inset::Motion typed;
        typed.in = {inset::Motion::Kind::Type, inset::Motion::Side::Left, 1200};
        typed.out = {inset::Motion::Kind::Type, inset::Motion::Side::Left, 400};
        tl->setTrackClipMotion(tl->trackAt(0).clips.at(0).id, typed);
        CHECK(p.save(file, &err));
        const QJsonObject jm = document()
                                   .value("timeline").toObject()
                                   .value("tracks").toArray().at(0).toObject()
                                   .value("clips").toArray().at(0).toObject()
                                   .value("motion").toObject();
        CHECK(jm.value("in").toObject().value("kind").toString() == "type" &&
              !jm.value("in").toObject().contains("side"));
        Project back;
        CHECK(back.load(file, &err));
        CHECK(back.timeline()->trackAt(0).clips.at(0).motion == typed);
        tl->setTrackClipMotion(tl->trackAt(0).clips.at(0).id, {});
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kTitleFormatVersion);
        CHECK(!document()
                   .value("timeline").toObject()
                   .value("tracks").toArray().at(0).toObject()
                   .value("clips").toArray().at(0).toObject()
                   .contains("motion"));
    }
    // A clock field (a brace in the text): written with the version that
    // added fields, read back as it was.
    {
        Project p;
        CHECK(p.load(file, &err));
        Timeline *tl = p.timeline();
        title::Style clock = *tl->trackAt(0).clips.at(0).title;
        clock.text = "{time:ss} {{ok}";
        clock.hours = title::Hours::H24;
        tl->setTrackClipTitle(tl->trackAt(0).clips.at(0).id, clock);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kClockFormatVersion);
        Project back;
        CHECK(back.load(file, &err));
        CHECK(back.timeline()->trackAt(0).clips.at(0).title->text == "{time:ss} {{ok}");
        CHECK(back.timeline()->trackAt(0).clips.at(0).title->hours == title::Hours::H24);
        // Metric units aren't written; others are, and read back.
        CHECK(!document().value("timeline").toObject().value("tracks").toArray().at(0)
                   .toObject().value("clips").toArray().at(0).toObject().value("title")
                   .toObject().contains("speedUnit"));
        clock.text = "{speed} {altitude}";
        clock.units = {title::SpeedUnit::Mph, title::AltitudeUnit::Feet};
        tl->setTrackClipTitle(tl->trackAt(0).clips.at(0).id, clock);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kClockFormatVersion);
        Project gps;
        CHECK(gps.load(file, &err));
        const title::Style &g = *gps.timeline()->trackAt(0).clips.at(0).title;
        CHECK(g.text == "{speed} {altitude}" && g.units == clock.units);
        // A unit change is an undo step like any edit.
        title::Style metric = g;
        metric.units = {};
        gps.timeline()->setTrackClipTitle(gps.timeline()->trackAt(0).clips.at(0).id, metric);
        CHECK(gps.canUndo());
        gps.undo();
        CHECK(gps.timeline()->trackAt(0).clips.at(0).title->units == clock.units);
    }
    // A brace in an older file's title is a brace: escaped, so it shows as
    // it did.
    {
        QJsonObject doc = document();
        doc["version"] = Project::kTitleLookFormatVersion;
        QJsonObject tlj = doc.value("timeline").toObject();
        QJsonArray tracks = tlj.value("tracks").toArray();
        QJsonObject track = tracks.at(0).toObject();
        QJsonArray clips = track.value("clips").toArray();
        QJsonObject clip = clips.at(0).toObject();
        QJsonObject jt = clip.value("title").toObject();
        jt["text"] = "Price {time} {{";
        clip["title"] = jt;
        clips[0] = clip;
        track["clips"] = clips;
        tracks[0] = track;
        tlj["tracks"] = tracks;
        doc["timeline"] = tlj;
        writeFile(file, QJsonDocument(doc).toJson());
        Project p;
        CHECK(p.load(file, &err));
        const QString text = p.timeline()->trackAt(0).clips.at(0).title->text;
        CHECK(text == "Price {{time} {{{{");
        CHECK(title::withFields(text, QDateTime()) == "Price {time} {{");
    }
}

// A gauge: written with its style in place of a source, with the version
// that added gauges, read back as it was; not a media pool item.
void testGaugeJson()
{
    QTemporaryDir tmp;
    const QString file = tmp.filePath("g.orp");
    QString err;
    gauge::Style g;
    g.kind = gauge::Kind::Tape;
    g.reads = gauge::Reads::Altitude;
    g.units = {title::SpeedUnit::Mph, title::AltitudeUnit::Feet};
    g.min = -100.0;
    g.max = 4000.0;
    g.accent = 0x12ab34;
    g.face = 0x202020;
    g.faceOpacity = 0.25f;
    g.text = 0xfafafa;
    g.font = "DejaVu Sans";
    {
        Project p;
        p.timeline()->appendClip("a.insv", 60000, 30.0);
        useOldStitcher(*p.timeline());
        const int v = p.timeline()->addTrack(Track::Kind::Video);
        inset::Look look = gauge::defaultLook();
        look.centerX = 0.3f;
        CHECK(p.timeline()->addGaugeClip(v, 1500, g, 8000, look));
        CHECK(p.save(file, &err));
    }
    QFile f(file);
    CHECK(f.open(QIODevice::ReadOnly));
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    CHECK(doc.value("version").toInt() == Project::kGaugeFormatVersion);
    const QJsonObject clip = doc.value("timeline").toObject().value("tracks").toArray().at(0)
                                 .toObject().value("clips").toArray().at(0).toObject();
    CHECK(clip.contains("gauge") && !clip.contains("source"));
    CHECK(clip.value("gauge").toObject().value("kind").toString() == "tape");
    Project back;
    CHECK(back.load(file, &err));
    const TrackClip &c = back.timeline()->trackAt(0).clips.at(0);
    CHECK(c.isGauge() && *c.gauge == g && c.startMs == 1500 && c.timelineDurationMs() == 8000);
    CHECK(c.look.centerX == 0.3f && c.sourcePath.isEmpty() && !c.mediaMissing);
    CHECK(back.mediaPool()->items().size() == 1); // the main clip only

    // The lean dial, the G circle and the route need version 20, as do the
    // lean and G fields; their scales read back the same either side of 0.
    for (const gauge::Kind kind : {gauge::Kind::Lean, gauge::Kind::GForce, gauge::Kind::Route}) {
        gauge::Style m;
        m.kind = kind;
        if (kind == gauge::Kind::Route) { // written only for a route
            m.routeView = gauge::RouteView::Trip;
            m.aheadS = 120.0;
        }
        const gauge::Range r = gauge::defaultRange(kind, m.reads, m.units);
        m.min = r.min;
        m.max = r.max;
        m = m.clamped();
        Project p;
        p.timeline()->appendClip("a.insv", 60000, 30.0);
        useOldStitcher(*p.timeline());
        const int v = p.timeline()->addTrack(Track::Kind::Video);
        CHECK(p.timeline()->addGaugeClip(v, 0, m, 5000));
        CHECK(p.save(file, &err));
        QFile mf(file);
        CHECK(mf.open(QIODevice::ReadOnly));
        CHECK(QJsonDocument::fromJson(mf.readAll()).object().value("version").toInt() ==
              Project::kMotionOverlayFormatVersion);
        Project mb;
        CHECK(mb.load(file, &err));
        CHECK(*mb.timeline()->trackAt(0).clips.at(0).gauge == m);
    }
    for (const QString &text : {QStringLiteral("{lean}"), QStringLiteral("{g}"),
                                QStringLiteral("{{lean} {speed}")}) {
        Project p;
        p.timeline()->appendClip("a.insv", 60000, 30.0);
        useOldStitcher(*p.timeline());
        const int v = p.timeline()->addTrack(Track::Kind::Video);
        title::Style t;
        t.text = text;
        CHECK(p.timeline()->addTitleClip(v, 0, t, 5000));
        CHECK(p.save(file, &err));
        QFile tf(file);
        CHECK(tf.open(QIODevice::ReadOnly));
        CHECK(QJsonDocument::fromJson(tf.readAll()).object().value("version").toInt() ==
              (text.startsWith("{{") ? Project::kClockFormatVersion
                                     : Project::kMotionOverlayFormatVersion));
    }
}

// Transitions: saved at their cut (the index of the clip before it), with
// the version that added them; loaded back; an undo step like any edit.
void testTransitionJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString video = tmp.path() + "/clip.insv";
    writeFile(video);
    const QString file = tmp.path() + "/transitions.orp";
    auto document = [&file] {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };
    QString err;
    {
        Project p;
        Timeline *tl = p.timeline();
        tl->appendClip(video, 10000, 30.0, 7680, 3840, 0);
        const int b = tl->appendClip(video, 10000, 30.0, 7680, 3840, 0);
        tl->appendClip(video, 10000, 30.0, 7680, 3840, 0);
        useOldStitcher(*tl);
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kPlainFormatVersion);
        CHECK(!document().value("timeline").toObject().contains("transitions"));
        tl->setTransition(b, Transition::Type::Crossfade, 1500);
        CHECK(p.modified());
        CHECK(p.save(file, &err));
        CHECK(document().value("version").toInt() == Project::kTransitionFormatVersion);
        const QJsonArray jt = document().value("timeline").toObject().value("transitions").toArray();
        CHECK(jt.size() == 1);
        CHECK(jt.at(0).toObject().value("cut").toInt() == 1);
        CHECK(jt.at(0).toObject().value("type").toString() == "crossfade");
        CHECK(jt.at(0).toObject().value("durationMs").toInt() == 1500);
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        const Timeline *tl = p.timeline();
        CHECK(tl->transitions().size() == 1);
        const Transition *t = tl->transitionAfter(tl->clipAt(1).id);
        CHECK(t && t->rightClipId == tl->clipAt(2).id && t->durationMs == 1500);
        CHECK(!p.modified());
        // Removing it is undone like any edit.
        p.timeline()->removeTransition(tl->clipAt(1).id);
        CHECK(tl->transitions().isEmpty() && p.canUndo());
        p.undo();
        CHECK(p.timeline()->transitions().size() == 1);
        // A dip to black by its name.
        p.timeline()->setTransition(p.timeline()->clipAt(1).id, Transition::Type::DipToBlack, 800);
        CHECK(p.save(file, &err));
        const QJsonArray jt = document().value("timeline").toObject().value("transitions").toArray();
        CHECK(jt.size() == 1 && jt.at(0).toObject().value("type").toString() == "dipToBlack");
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        const Transition *t = p.timeline()->transitionAfter(p.timeline()->clipAt(1).id);
        CHECK(t && t->type == Transition::Type::DipToBlack && t->durationMs == 800);
    }
}

// A clip's LUT: copied into the project folder's luts/ (the same file
// reused, a different one with the same name numbered), stored relative to
// the project, a version-14 file only when a clip has one, and brought along
// when the project is duplicated.
void testLutJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString video = tmp.path() + "/clip.insv";
    writeFile(video);
    const QString downloads = tmp.path() + "/Downloads";
    QDir().mkpath(downloads);
    auto writeLut = [](const QString &path, const char *text) {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(text);
    };
    writeLut(downloads + "/X5 Log.cube", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
                                         "0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    QString err;
    const QString file = projectfolder::create(tmp.path(), "Ride", &err);
    CHECK(!file.isEmpty());
    CHECK(projectfolder::lutsDir(file) == tmp.path() + "/Ride/luts");

    const QString copy = projectfolder::importLut(file, downloads + "/X5 Log.cube", &err);
    CHECK(copy == tmp.path() + "/Ride/luts/X5 Log.cube" && QFileInfo::exists(copy));
    CHECK(projectfolder::importLut(file, downloads + "/X5 Log.cube") == copy); // same file
    CHECK(projectfolder::importLut(file, copy) == copy);                      // already there
    QDir().mkpath(downloads + "/other");
    writeLut(downloads + "/other/X5 Log.cube", "LUT_3D_SIZE 2\n");
    CHECK(projectfolder::importLut(file, downloads + "/other/X5 Log.cube") ==
          tmp.path() + "/Ride/luts/X5 Log 2.cube");
    // A loose or untitled project keeps the file where it is.
    CHECK(projectfolder::importLut(tmp.path() + "/loose.orp", downloads + "/X5 Log.cube") ==
          downloads + "/X5 Log.cube");
    CHECK(projectfolder::importLut(QString(), downloads + "/X5 Log.cube") ==
          downloads + "/X5 Log.cube");

    auto document = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };
    auto firstColor = [&](const QString &path) {
        return document(path)
            .value("timeline").toObject().value("clips").toArray().at(0).toObject()
            .value("color").toObject();
    };
    {
        Project p;
        const int id = p.timeline()->appendClip(video, 10000, 30.0, 7680, 3840, 0);
        useOldStitcher(*p.timeline());
        CHECK(p.save(file, &err));
        CHECK(document(file).value("version").toInt() == Project::kPlainFormatVersion);
        CHECK(!firstColor(file).contains("lut"));
        ColorGrade g;
        g.lut = copy;
        g.lutStrength = 75.0f;
        g.contrast = 10.0f;
        p.timeline()->setClipColor(id, g);
        CHECK(p.save(file, &err));
        CHECK(document(file).value("version").toInt() == Project::kLutFormatVersion);
        CHECK(firstColor(file).value("lut").toString() == "luts/X5 Log.cube");
        CHECK(firstColor(file).value("lutStrength").toDouble() == 75.0);
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        const ColorGrade &g = p.timeline()->clipAt(0).color;
        CHECK(g.lut == copy && g.lutStrength == 75.0f && g.contrast == 10.0f);
    }
    // Duplicated: the copy has its own luts/, and its clip points there.
    const QString dup = projectfolder::duplicate(file, "Ride copy", &err);
    CHECK(!dup.isEmpty());
    CHECK(QFileInfo::exists(tmp.path() + "/Ride copy/luts/X5 Log.cube"));
    {
        Project p;
        CHECK(p.load(dup, &err));
        CHECK(p.timeline()->clipAt(0).color.lut == tmp.path() + "/Ride copy/luts/X5 Log.cube");
    }
}

void testSplitJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString video = tmp.path() + "/clip.insv";
    writeFile(video);
    const QString file = tmp.path() + "/split.orp";
    QString err;
    auto document = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                           : QJsonObject();
    };
    auto firstClip = [&](const QString &path) {
        return document(path).value("timeline").toObject().value("clips").toArray().at(0).toObject();
    };
    {
        Project p;
        const int id = p.timeline()->appendClip(video, 10000, 30.0, 7680, 3840, 0);
        useOldStitcher(*p.timeline());
        CHECK(p.save(file, &err));
        CHECK(document(file).value("version").toInt() == Project::kPlainFormatVersion);
        CHECK(!firstClip(file).contains("split"));

        pane::Split grid;
        grid.layout = pane::Layout::Grid;
        grid.gap = 0.01f;
        grid.gapRgb = 0x204060;
        const std::vector<Timeline::PanePose> poses{{0, {0.1f, 0.0f, 0.0f, 1.0f}},
                                                    {1, {0.2f, 0.1f, 0.0f, 1.1f}},
                                                    {2, {0.3f, 0.0f, 0.2f, 1.2f}},
                                                    {3, {0.4f, -0.1f, 0.0f, 1.3f}}};
        p.timeline()->setClipSplit(id, grid, 2000, &poses);
        p.timeline()->upsertClipReframeKey(id, 5000, {1.0f, 0.0f, 0.0f, 0.9f}, 2);
        p.timeline()->setClipReframeEase(id, 2000, keytrack::Ease::Out, 2);
        const ClipItem &c = p.timeline()->clipAt(0);
        CHECK(c.split == grid && c.paneCount() == 4);
        CHECK(c.reframe.count() == 1 && c.reframe.keys()[0].pose.yaw == 0.1f);
        CHECK(c.panes[1].count() == 2 && c.panes[2].count() == 1);
        CHECK(p.save(file, &err));
        // Split, with keys holding the diagonal field of view.
        CHECK(document(file).value("version").toInt() == Project::kWideViewFormatVersion);
        const QJsonObject js = firstClip(file).value("split").toObject();
        CHECK(js.value("layout").toString() == "grid");
        CHECK(js.value("gapColor").toString() == "#204060");
        CHECK(js.value("panes").toArray().size() == 3);
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        const ClipItem &c = p.timeline()->clipAt(0);
        CHECK(c.split.layout == pane::Layout::Grid && c.split.gap == 0.01f &&
              c.split.gapRgb == 0x204060);
        CHECK(c.reframe.count() == 1 && c.reframe.keys()[0].pose.yaw == 0.1f);
        CHECK(c.panes[0].count() == 1 && c.panes[0].keys()[0].pose.fov == 1.1f);
        CHECK(c.panes[1].count() == 2 && c.panes[1].keys()[0].ease == keytrack::Ease::Out);
        CHECK(c.panes[1].keys()[1].pose.yaw == 1.0f);
        CHECK(c.panes[2].keys()[0].pose.pitch == -0.1f);

        // Back to one picture: the pane cameras stay (in the file too).
        p.timeline()->setClipSplit(c.id, pane::Split{});
        CHECK(p.timeline()->clipAt(0).paneCount() == 1);
        CHECK(p.timeline()->clipAt(0).panes[1].count() == 2);
        CHECK(p.save(file, &err));
        CHECK(document(file).value("version").toInt() == Project::kWideViewFormatVersion);
        CHECK(firstClip(file).value("split").toObject().value("layout").toString() == "single");

        // Undo brings the grid back.
        p.undo();
        CHECK(p.timeline()->clipAt(0).split.layout == pane::Layout::Grid);
    }
    {
        // An unknown layout (a newer build's) reads as no split.
        Project p;
        CHECK(p.load(file, &err));
        CHECK(p.timeline()->clipAt(0).panes[1].count() == 2);
        pane::Split side;
        side.layout = pane::Layout::SideBySide;
        p.timeline()->setClipSplit(p.timeline()->clipAt(0).id, side);
        CHECK(p.save(file, &err));
        QFile f(file);
        CHECK(f.open(QIODevice::ReadOnly));
        QByteArray text = f.readAll();
        f.close();
        text.replace("\"sideBySide\"", "\"hexagon\"");
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
        f.close();
        Project q;
        CHECK(q.load(file, &err));
        CHECK(q.timeline()->clipAt(0).split.layout == pane::Layout::Single);
    }
}

// Version 17: keys hold the diagonal field of view and the distortion. An
// older file's vertical angle becomes the diagonal that frames the same
// corners at the output's aspect (or its pane's), with Auto.
void testWideViewJson()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    writeFile(tmp.path() + "/clip.insv");
    const QString old = tmp.path() + "/old.orp";
    writeFile(old, QStringLiteral(R"({"format": "orbis-reframe-project", "version": 16,
        "output": {"aspect": "9:16", "width": 1080, "height": 1920, "fps": 0},
        "timeline": {"clips": [
          {"source": "clip.insv", "trimInMs": 0, "trimOutMs": 5000, "sourceDurationMs": 5000,
           "fps": 30, "reframe": {"keys": [{"tMs": 1000, "yaw": 0.5, "pitch": 0, "roll": 0,
                                            "fovY": 1.309}]},
           "split": {"layout": "stacked", "gap": 0, "gapColor": "#000000", "panes": [
             {"keys": [{"tMs": 0, "yaw": 3.0, "pitch": 0, "roll": 0, "fovY": 1.0}]},
             {"keys": []}, {"keys": []}]}}]}})")
                       .toUtf8());
    QString err;
    const QString file = tmp.path() + "/new.orp";
    {
        Project p;
        CHECK(p.load(old, &err));
        const ClipItem &c = p.timeline()->clipAt(0);
        CHECK(c.reframe.count() == 1 && c.panes[0].count() == 1);
        if (c.reframe.count() != 1 || c.panes[0].count() != 1)
            return;
        const ReframePose &k = c.reframe.keys()[0].pose;
        CHECK(k.fov == view::fovFor<double>(1.309f, 0.0, 1080.0 / 1920.0) && k.distortionIsAuto());
        // At d = 0 it is the same picture, to the bit.
        view::Pose rect = k;
        rect.distortion = 0.0f;
        CHECK(view::lens(rect, 1080.0 / 1920.0).fovY == 1.309f);
        // The bottom pane's key at its own aspect.
        pane::Split stacked;
        stacked.layout = pane::Layout::Stacked;
        stacked.gap = 0.0f;
        const double paneAspect = pane::rects(stacked, 1080, 1920)[1].aspect();
        const ReframePose &pk = c.panes[0].keys()[0].pose;
        CHECK(pk.fov == view::fovFor<double>(1.0f, 0.0, paneAspect));
        CHECK(paneAspect > 1080.0 / 1920.0);

        ReframePose fixed = k;
        fixed.distortion = 0.35f;
        p.timeline()->upsertClipReframeKey(c.id, 3000, fixed);
        CHECK(p.save(file, &err));
    }
    {
        QFile f(file);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
        CHECK(doc.value("version").toInt() == Project::kWideViewFormatVersion);
        const QJsonArray keys = doc.value("timeline").toObject().value("clips").toArray().at(0)
                                    .toObject().value("reframe").toObject().value("keys").toArray();
        CHECK(keys.size() == 2);
        const QJsonObject k0 = keys.at(0).toObject(), k1 = keys.at(1).toObject();
        CHECK(k0.contains("diag") && !k0.contains("fovY") && !k0.contains("distortion"));
        CHECK(std::abs(k1.value("distortion").toDouble() - 0.35) < 1e-6);
    }
    {
        Project p;
        CHECK(p.load(file, &err));
        const ClipItem &c = p.timeline()->clipAt(0);
        CHECK(c.reframe.count() == 2);
        if (c.reframe.count() != 2)
            return;
        // The diagonal round trips exactly (a double in the file).
        CHECK(c.reframe.keys()[0].pose.fov == view::fovFor<double>(1.309f, 0.0, 1080.0 / 1920.0));
        CHECK(c.reframe.keys()[0].pose.distortionIsAuto());
        CHECK(c.reframe.keys()[1].pose.distortion == 0.35f);
        CHECK(c.panes[0].keys()[0].pose.fov == view::fovFor<double>(
                  1.0f, 0.0, pane::rects(c.split, 1080, 1920)[1].aspect()));
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); // Project's timers want one (never run here)
    testProjectJson();
    testProjectUndoRedo();
    testExportBitrate();
    testProjectConvertSound();
    testProjectTracksJson();
    testProjectVideoTracksJson();
    testMediaPool();
    testTakeFiles();
    testRecordingParts();
    testProjectFolder();
    testAutosave();
    testMeasurementsWritten();
    testOldSeamMeasurement();
    testLensColourMethod();
    testProjectLibrary();
    testProjectSummary();
    testFlatClipJson();
    testClipStitchJson();
    testMarksJson();
    testMediaMarks();
    testLensCalibrationJson();
    testLensCalibrationStore();
    testPlateScansInUse();
    testTitleJson();
    testGaugeJson();
    testTransitionJson();
    testLutJson();
    testSplitJson();
    testWideViewJson();
    return modeltest::finish("project_test");
}
