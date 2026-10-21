#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <AudioPeaks.h> // insv::AudioPeaks

#include <atomic>
#include <memory>

// Waveforms for the timeline: an audio stream's peaks, computed once on a
// background thread (insv::ComputeAudioPeaks) and kept in memory and on disk
// (keyed by path + size + mtime + stream; the open project's cache/ folder,
// or the user cache for an untitled or loose project), so reopening a
// project doesn't rescan. peaks() never blocks: it returns null and starts the
// scan, and ready() fires when it's done.
class WaveformCache : public QObject {
    Q_OBJECT

public:
    explicit WaveformCache(QObject *parent = nullptr);
    ~WaveformCache() override; // cancels and waits for running scans

    using Peaks = std::shared_ptr<const insv::AudioPeaks>;
    // `stream` as InsvAudioDemux::open's track (-1 = default). Null until
    // it's been scanned; an empty result (no audio) is a non-null, empty one.
    Peaks peaks(const QString &path, int stream);
    // Scan these files again on the next peaks(): replaced in place, or
    // back after they were gone. (On disk a changed file has a new key.)
    void forget(const QStringList &paths);
    // The folder waveforms are kept in (waveforms/ is added); empty = the
    // user cache. What's in memory stays (it's the same audio).
    void setDirectory(const QString &dir) { m_dir = dir; }

signals:
    void ready(); // some waveform arrived: redraw

private:
    static QString key(const QString &path, int stream);
    QString diskPath(const QString &path, int stream) const;

    QString m_dir;
    QHash<QString, Peaks> m_done;
    QSet<QString> m_pending;
    QThreadPool m_pool;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};
