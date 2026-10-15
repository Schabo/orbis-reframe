#include "TrackFiles.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QList>
#include <QMessageBox>

#include <algorithm>
#include <string>
#include <utility>

#include <FlatVideoReader.h> // exportpipe::FlatVideoReader::probe
#include <InsvAudioDemux.h>  // insv::InsvAudioDemux

#include "../core/MediaPool.h"
#include "../core/Project.h"
#include "../core/Timeline.h"

TrackFiles::TrackFiles(Project *project, std::function<void(int)> addOverlaySound,
                       QWidget *window)
    : m_project(project), m_addOverlaySound(std::move(addOverlaySound)), m_window(window)
{
}

std::optional<int> TrackFiles::pickAudio(int trackId, qint64 atMs)
{
    if (m_project->timeline()->count() == 0)
        return std::nullopt; // the main track sets the timeline's length: nothing to lay audio under
    const QStringList paths = QFileDialog::getOpenFileNames(
        m_window, tr("Add audio files"),
        m_lastAudioDir.isEmpty() ? QDir::homePath() : m_lastAudioDir,
        tr("Audio files (*.mp3 *.wav *.flac *.aac *.m4a *.ogg *.opus);;All files (*)"));
    if (paths.isEmpty())
        return std::nullopt;
    m_lastAudioDir = QFileInfo(paths.first()).absolutePath();
    return addAudio(paths, trackId, atMs);
}

std::optional<int> TrackFiles::addAudio(const QStringList &paths, int trackId, qint64 atMs)
{
    Timeline *timeline = m_project->timeline();
    if (timeline->count() == 0)
        return std::nullopt;
    // Probe them all first, so the track picked has room for the lot.
    QList<std::pair<QString, qint64>> ok;
    QStringList failed;
    qint64 total = 0;
    for (const QString &path : paths) {
        insv::InsvAudioDemux probe;
        std::string err;
        if (!probe.open(path.toStdString(), 48000, 2, err) || probe.durationMs() <= 0) {
            failed << QFileInfo(path).fileName() +
                          (err.empty() ? QString() : QStringLiteral(": ") + QString::fromStdString(err));
            continue;
        }
        ok.append({path, probe.durationMs()});
        total += probe.durationMs();
    }
    if (!failed.isEmpty())
        QMessageBox::warning(m_window, tr("Could not add audio"),
                             tr("No audio found in:\n%1").arg(failed.join(QLatin1Char('\n'))));
    if (ok.isEmpty())
        return std::nullopt;
    atMs = std::clamp<qint64>(atMs, 0, timeline->totalDurationMs());

    // No track of the Audio group given (the others hold a video's own
    // sound): the first there with room for them, else a new one. Several
    // files go one after another.
    const Track *given = trackId > 0 ? timeline->trackById(trackId) : nullptr;
    if (!given || given->group() != Track::Group::Audio) {
        trackId = timeline->freeTrackFor(atMs, atMs + total, false, [](const Track &t) {
            return t.group() == Track::Group::Audio;
        });
        if (!trackId)
            trackId = timeline->addTrack(Track::Kind::Audio);
    }
    int last = 0;
    for (const auto &[path, durMs] : ok) {
        last = timeline->addAudioClip(trackId, path, durMs, atMs);
        atMs += durMs;
    }
    return last;
}

std::optional<int> TrackFiles::pickVideo(int trackId, qint64 atMs)
{
    if (m_project->timeline()->count() == 0)
        return std::nullopt; // the main track sets the timeline's length: nothing to lay an overlay over
    const QStringList paths = QFileDialog::getOpenFileNames(
        m_window, tr("Add video overlay"),
        m_lastVideoDir.isEmpty() ? QDir::homePath() : m_lastVideoDir,
        tr("Video files (*.mp4 *.mov *.mkv *.m4v *.webm *.avi);;All files (*)"));
    if (paths.isEmpty())
        return std::nullopt;
    m_lastVideoDir = QFileInfo(paths.first()).absolutePath();
    return addVideo(paths, trackId, atMs);
}

std::optional<int> TrackFiles::addVideo(const QStringList &paths, int trackId, qint64 atMs)
{
    Timeline *timeline = m_project->timeline();
    if (timeline->count() == 0)
        return std::nullopt;
    struct Probed {
        QString path;
        exportpipe::FlatVideoReader::Info info;
    };
    QList<Probed> ok;
    QStringList failed;
    qint64 total = 0;
    for (const QString &path : paths) {
        Probed p{path, {}};
        std::string err;
        if (!exportpipe::FlatVideoReader::probe(path.toStdString(), p.info, err)) {
            failed << QFileInfo(path).fileName() +
                          (err.empty() ? QString() : QStringLiteral(": ") + QString::fromStdString(err));
            continue;
        }
        ok.append(p);
        total += p.info.durationMs;
    }
    if (!failed.isEmpty())
        QMessageBox::warning(m_window, tr("Could not add video"),
                             tr("No video found in:\n%1").arg(failed.join(QLatin1Char('\n'))));
    if (ok.isEmpty())
        return std::nullopt;
    atMs = std::clamp<qint64>(atMs, 0, timeline->totalDurationMs());

    // No overlay track given: the first with room for them there, else a new
    // one. Several files go one after another.
    const Track *given = trackId > 0 ? timeline->trackById(trackId) : nullptr;
    if (!given || given->kind != Track::Kind::Video) {
        trackId = timeline->freeTrackFor(atMs, atMs + total, true, [](const Track &) { return true; });
        if (!trackId)
            trackId = timeline->addTrack(Track::Kind::Video);
    }
    int last = 0;
    for (const Probed &p : ok) {
        // Into the pool from the probe first: the timeline's clip doesn't
        // keep the frame rate, so the pool's own fill-in would go without.
        m_project->mediaPool()->add({p.path, MediaItem::Kind::Video, p.info.durationMs,
                                     p.info.width, p.info.height, p.info.fps});
        last = timeline->addVideoClip(trackId, p.path, p.info.durationMs, atMs, p.info.width,
                                      p.info.height);
        m_addOverlaySound(last); // its sound, linked, like a video clip's
        atMs += p.info.durationMs;
    }
    return last;
}
