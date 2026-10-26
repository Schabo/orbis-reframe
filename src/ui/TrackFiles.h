#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class Project;
class QWidget;

// Putting audio files and overlay videos onto the timeline's tracks: Add
// audio file… / Add picture in picture… (a file picker), drops and the Media tab
// (paths). Files are probed first; the ones without audio / video are
// reported and skipped, the rest go one after another from `atMs` (clamped
// to the timeline) on track `trackId`, or -- when that isn't a track of the
// right kind -- the first with room for them all, else a new one. Nothing
// goes on an empty timeline: the main track sets its length.
//
// Each returns the last clip added, to select; nothing when none was.
class TrackFiles {
    Q_DECLARE_TR_FUNCTIONS(TrackFiles)

public:
    // `addOverlaySound`: puts a new overlay clip's sound on its audio track
    // (MainWindow checks the file has sound). `window`: the dialogs' parent.
    TrackFiles(Project *project, std::function<void(int overlayClipId)> addOverlaySound,
               QWidget *window);

    std::optional<int> pickAudio(int trackId, qint64 atMs);
    std::optional<int> addAudio(const QStringList &paths, int trackId, qint64 atMs);
    std::optional<int> pickVideo(int trackId, qint64 atMs);
    // Also into the media pool, with the frame rate the probe found.
    std::optional<int> addVideo(const QStringList &paths, int trackId, qint64 atMs);

private:
    Project *m_project;
    std::function<void(int)> m_addOverlaySound;
    QWidget *m_window;
    QString m_lastAudioDir; // where the last audio file was picked from
    QString m_lastVideoDir; // ... and the last overlay video
};
