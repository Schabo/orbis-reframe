#include "RenderJobs.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include <InsvTrailer.h> // insv::ReadInsvTelemetry

#include "PrivacyBlur.h"
#include "../core/ClipItem.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"

namespace renderjobs {

std::vector<insv::MixClip> mixClips(const Timeline &timeline)
{
    constexpr int kFramesPerMs = 48; // playback and export both mix at 48 kHz
    std::vector<insv::MixClip> mix;
    for (int i = 0; i < timeline.trackCount(); ++i) {
        const Track &t = timeline.trackAt(i);
        if (t.muted || t.kind != Track::Kind::Audio)
            continue;
        for (const TrackClip &c : t.clips) {
            if (c.muted || c.mediaMissing)
                continue;
            insv::MixClip m;
            m.path = c.sourcePath.toStdString();
            m.stream = c.stream;
            m.startFrame = c.startMs * kFramesPerMs;
            m.lengthFrames = c.timelineDurationMs() * kFramesPerMs;
            m.inFrame = c.trimInMs * kFramesPerMs;
            m.speed = c.speed;
            m.gain = static_cast<float>(std::pow(10.0, (c.gainDb + t.gainDb) / 20.0));
            m.fadeInFrames = c.fadeInMs * kFramesPerMs;
            m.fadeOutFrames = c.fadeOutMs * kFramesPerMs;
            m.noise = c.noise;
            mix.push_back(std::move(m));
        }
    }
    return mix;
}

std::vector<InsetFeed::Clip> insetFeed(const Timeline &timeline)
{
    std::vector<InsetFeed::Clip> clips;
    // Bottom-most first: the lowest overlay row draws first, the top row last.
    for (int i = timeline.trackCount() - 1; i >= 0; --i) {
        const Track &t = timeline.trackAt(i);
        if (t.kind != Track::Kind::Video || t.muted)
            continue; // (hidden)
        for (const TrackClip &c : t.clips) {
            if (c.mediaMissing)
                continue;
            InsetFeed::Clip f;
            f.id = c.id;
            f.path = c.sourcePath;
            f.startMs = c.startMs;
            f.trimInMs = c.trimInMs;
            f.trimOutMs = c.trimOutMs;
            f.speed = c.speed;
            f.aspect = c.aspect();
            f.fadeInMs = c.fadeInMs;
            f.fadeOutMs = c.fadeOutMs;
            f.look = c.look;
            f.frame = c.frame;
            f.keys = c.insetKeys;
            clips.push_back(std::move(f));
        }
    }
    return clips;
}

std::vector<exportpipe::InsetClipJob> insetJobs(const Timeline &timeline)
{
    std::vector<exportpipe::InsetClipJob> jobs;
    for (int i = timeline.trackCount() - 1; i >= 0; --i) {
        const Track &t = timeline.trackAt(i);
        if (t.kind != Track::Kind::Video || t.muted)
            continue;
        for (const TrackClip &c : t.clips) {
            if (c.mediaMissing)
                continue;
            exportpipe::InsetClipJob j;
            j.path = c.sourcePath.toStdString();
            j.startMs = c.startMs;
            j.trimInMs = c.trimInMs;
            j.trimOutMs = c.trimOutMs;
            j.speed = c.speed;
            j.aspect = c.aspect();
            j.fadeInMs = c.fadeInMs;
            j.fadeOutMs = c.fadeOutMs;
            j.frame = c.frame;
            // A copy: the export thread samples it.
            j.lookAt = [look = c.look, keys = c.insetKeys](int64_t sourceMs) {
                return keys.isEmpty() ? look : keys.sample(sourceMs);
            };
            jobs.push_back(std::move(j));
        }
    }
    return jobs;
}

ReframeExportController::ClipJob clipJob(const ClipItem &c, const Timeline &timeline,
                                         int lensGuard, const PrivacyBlur &privacy)
{
    // Stabilization (Lean follow / Horizon Lock / Custom) is a per-frame
    // sphere-frame matrix next to the reframe pose, applied after it like the
    // viewport's pc.stab -- the native stitch itself is never stabilized.
    const bool nativeStab = c.stabilization.mode != insv::StabMode::Off;
    ReframeExportController::ClipJob job;
    job.clipPath = c.sourcePath;
    job.trimInMs = c.trimInMs;
    job.trimOutMs = c.trimOutMs;
    job.speed = c.speed;
    const std::array<float, 4> grade = c.color.packed();
    std::copy(grade.begin(), grade.end(), job.colorGrade);
    job.blur = privacy.blurFor(c);
    timeline.sourceSeamAdjust(c.sourcePath, lensGuard, job.seamAdjustDeg);
    if (nativeStab) {
        auto tele = insv::ReadInsvTelemetry(c.sourcePath.toStdString());
        if (tele && tele->imu.size() >= 2) {
            auto stab = std::make_shared<insv::GyroStabilizer>();
            const double syncMs = std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs;
            stab->build(*tele, c.fps, c.stabilization, syncMs / 1000.0);
            if (stab->ready())
                job.stab = stab;
        }
    }
    // No keyframes -> a static forward view.
    if (!c.reframe.isEmpty())
        job.poseAt = [track = c.reframe](qint64 sourceMs, float pose[4]) {
            const ReframePose p = track.sample(sourceMs);
            pose[0] = p.yaw;
            pose[1] = p.pitch;
            pose[2] = p.roll;
            pose[3] = p.fovY;
        };
    return job;
}

QString suggestedExportPath(const QString &projectFile)
{
    if (projectFile.isEmpty())
        return QDir::homePath() + QStringLiteral("/timeline.mp4");
    QString dir = projectfolder::ensure(projectfolder::exportsDir(projectFile));
    if (dir.isEmpty())
        dir = QFileInfo(projectFile).absolutePath();
    const QString base = QFileInfo(projectFile).completeBaseName();
    QString suggested = QDir(dir).filePath(base + QStringLiteral(".mp4"));
    for (int n = 2; QFileInfo::exists(suggested); ++n)
        suggested = QDir(dir).filePath(QStringLiteral("%1-%2.mp4").arg(base).arg(n));
    return suggested;
}

QString suggestedScreenshotPath(const QString &projectFile, const QString &clipSource,
                                qint64 timelineMs)
{
    QString dir;
    QString base;
    if (!projectFile.isEmpty()) {
        const QFileInfo fi(projectFile);
        dir = projectfolder::ensure(projectfolder::screenshotsDir(projectFile));
        if (dir.isEmpty())
            dir = fi.absolutePath();
        base = fi.completeBaseName();
    } else {
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (dir.isEmpty() || !QDir(dir).exists())
            dir = QDir::homePath();
        base = QFileInfo(clipSource).completeBaseName();
    }
    const qint64 t = std::max<qint64>(0, timelineMs);
    const QString stamp = QStringLiteral("%1m%2s%3")
                              .arg(t / 60000, 2, 10, QLatin1Char('0'))
                              .arg(t / 1000 % 60, 2, 10, QLatin1Char('0'))
                              .arg(t % 1000, 3, 10, QLatin1Char('0'));
    QString suggested = QDir(dir).filePath(QStringLiteral("%1_%2.png").arg(base, stamp));
    for (int n = 2; QFileInfo::exists(suggested); ++n)
        suggested = QDir(dir).filePath(QStringLiteral("%1_%2-%3.png").arg(base, stamp).arg(n));
    return suggested;
}

} // namespace renderjobs
