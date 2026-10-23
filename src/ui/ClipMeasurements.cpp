#include "ClipMeasurements.h"

#include <QThread>

#include <array>
#include <cmath>

#include <ClipSeam.h> // stitch::MeasureClipSeam
#ifdef ORBIS_REFRAME_HAVE_CLIP_SYNC
#include <ClipSync.h> // stitch::MeasureClipSync
#endif

#include "../core/ClipItem.h"
#include "../core/Log.h"
#include "../core/Project.h"
#include "../core/Timeline.h"

ClipMeasurements::ClipMeasurements(Project *project, bool enabled, QObject *parent)
    : QObject(parent), m_project(project), m_enabled(enabled)
{
}

ClipMeasurements::~ClipMeasurements()
{
    if (m_syncThread) {
        m_syncCancel = true;
        m_syncThread->wait();
    }
    if (m_seamThread) {
        m_seamCancel = true;
        m_seamThread->wait();
    }
}

const ClipItem *ClipMeasurements::measurableClip(const QString &clipPath) const
{
    const Timeline *t = m_project->timeline();
    for (int i = 0; i < t->count(); ++i)
        if (t->clipAt(i).sourcePath == clipPath) // a flat video has no IMU or seam
            return t->clipAt(i).mediaMissing || t->clipAt(i).flat ? nullptr : &t->clipAt(i);
    return nullptr;
}

void ClipMeasurements::schedule()
{
    if (m_queued)
        return;
    m_queued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_queued = false;
        if (m_playing)
            return; // picked up again on pause
        const Timeline *t = m_project->timeline();
        for (int i = 0; i < t->count(); ++i) {
            if (t->clipAt(i).flat)
                continue;
            const QString path = t->clipAt(i).sourcePath;
            maybeMeasureSync(path);
            maybeMeasureSeam(path);
        }
        emit statusChanged();
    }, Qt::QueuedConnection);
}

void ClipMeasurements::setPlaying(bool playing)
{
    m_playing = playing;
    if (playing) {
        // A measurement competes with playback for the GPU its whole run:
        // stop it; the pause starts it again.
        m_syncCancel = true;
        m_seamCancel = true;
        return;
    }
    schedule(); // measurements held back while playing
}

void ClipMeasurements::maybeMeasureSync(const QString &clipPath)
{
#ifdef ORBIS_REFRAME_HAVE_CLIP_SYNC
    if (!m_enabled || clipPath.isEmpty() || m_syncThread ||
        !std::isnan(m_project->timeline()->sourceSyncAdjust(clipPath)) ||
        m_syncFailed.contains(clipPath))
        return;
    const ClipItem *clip = measurableClip(clipPath);
    if (!clip)
        return;
    const std::string path = clipPath.toStdString();
    const qint64 in = clip->trimInMs, out = clip->trimOutMs;
    m_syncMeasuringPath = clipPath;
    m_syncCancel = false;
    const auto guard = clip->lensGuard;
    m_syncThread = QThread::create([this, path, in, out, clipPath, guard] {
        const stitch::ClipSyncResult r =
            stitch::MeasureClipSync(path, in, out, &m_syncCancel, guard);
        QMetaObject::invokeMethod(this, [this, clipPath, r] {
            m_syncThread->wait(); // its function has just returned
            m_syncThread->deleteLater();
            m_syncThread = nullptr;
            m_syncMeasuringPath.clear();
            core::log::Info("[sync] %s: %s\n", qPrintable(clipPath), r.message.c_str());
            if (r.ok)
                m_project->timeline()->setSourceSyncAdjust(clipPath, r.adjustMs); // -> clipSyncChanged
            else if (!m_syncCancel)
                m_syncFailed.insert(clipPath, r.failure);
            emit statusChanged();
            schedule(); // another source may be waiting for its turn
        }, Qt::QueuedConnection);
    });
    m_syncThread->start(QThread::LowPriority);
    core::log::Info("[sync] %s: measuring\n", qPrintable(clipPath));
    emit statusChanged();
#else
    Q_UNUSED(clipPath);
#endif
}

void ClipMeasurements::maybeMeasureSeam(const QString &clipPath)
{
    insv::SeamCorrection seam;
    if (!m_enabled || clipPath.isEmpty() || m_seamThread ||
        m_project->timeline()->sourceSeamCorrection(clipPath, seam) ||
        m_seamFailed.contains(clipPath))
        return;
    const ClipItem *clip = measurableClip(clipPath);
    if (!clip)
        return;
    const std::string path = clipPath.toStdString();
    const qint64 in = clip->trimInMs, out = clip->trimOutMs;
    const auto guard = clip->lensGuard;
    m_seamMeasuringPath = clipPath;
    m_seamCancel = false;
    m_seamThread = QThread::create([this, path, in, out, clipPath, guard] {
        const stitch::ClipSeamResult r = stitch::MeasureClipSeam(path, in, out, guard, &m_seamCancel);
        QMetaObject::invokeMethod(this, [this, clipPath, r, guard] {
            m_seamThread->wait(); // its function has just returned
            m_seamThread->deleteLater();
            m_seamThread = nullptr;
            m_seamMeasuringPath.clear();
            core::log::Info("[seam] %s: %s\n", qPrintable(clipPath), r.message.c_str());
            // Stale if the guard setting changed meanwhile: drop it and let the
            // re-measure below run under the new one.
            const ClipItem *now = measurableClip(clipPath);
            if (r.ok && now && guard == now->lensGuard)
                m_project->timeline()->setSourceSeamCorrection(
                    clipPath, r.correction, static_cast<int>(guard)); // -> clipSeamChanged
            else if (!r.ok && !m_seamCancel)
                m_seamFailed.insert(clipPath, r.failure);
            emit statusChanged();
            schedule(); // another source may be waiting for its turn
        }, Qt::QueuedConnection);
    });
    m_seamThread->start(QThread::LowPriority);
    core::log::Info("[seam] %s: measuring\n", qPrintable(clipPath));
}

QString ClipMeasurements::failureText(stitch::MeasureFailure why, bool seam)
{
    switch (why) {
    case stitch::MeasureFailure::NotCamera:
        return tr("not a 360 camera file");
    case stitch::MeasureFailure::Unsupported:
        return tr("this kind of 360 clip isn't supported");
    case stitch::MeasureFailure::NoGyro:
        return tr("the file has no usable gyroscope data");
    case stitch::MeasureFailure::TooLittleDetail:
        return seam ? tr("too little detail along the seam")
                    : tr("too little detail or movement in the picture");
    case stitch::MeasureFailure::None:
    case stitch::MeasureFailure::Cancelled: // not kept as a failure
    case stitch::MeasureFailure::Unreadable:
        break;
    }
    return tr("the file can't be read");
}

QString ClipMeasurements::seamStatus(const ClipItem *c) const
{
    insv::SeamCorrection seam;
    if (c && m_project->timeline()->sourceSeamCorrection(c->sourcePath, seam))
        return tr("Seam: aligned for this clip (back lens yaw %1°, pitch %2°, roll %3°).")
            .arg(seam.lens1RotDeg[0], 0, 'f', 2)
            .arg(seam.lens1RotDeg[1], 0, 'f', 2)
            .arg(seam.lens1RotDeg[2], 0, 'f', 2);
    if (c && m_seamMeasuringPath == c->sourcePath)
        return tr("Seam: measuring this clip…");
    if (const auto it = c ? m_seamFailed.constFind(c->sourcePath) : m_seamFailed.cend();
        it != m_seamFailed.cend())
        return tr("Seam: couldn't align this clip (%1).").arg(failureText(*it, true));
    return QString();
}

QString ClipMeasurements::syncStatus(const ClipItem *c) const
{
    if (!c || c->stabilization.mode == insv::StabMode::Off)
        return QString();
    const double ms = m_project->timeline()->sourceSyncAdjust(c->sourcePath);
    if (!std::isnan(ms))
        return tr("Gyro sync: fine-tuned for this clip (%1 ms).").arg(ms, 0, 'f', 1);
    if (m_syncMeasuringPath == c->sourcePath)
        return tr("Gyro sync: measuring this clip…");
    if (const auto it = m_syncFailed.constFind(c->sourcePath); it != m_syncFailed.cend())
        return tr("Gyro sync: couldn't fine-tune this clip (%1); using the camera's timing.")
            .arg(failureText(*it, false));
#ifndef ORBIS_REFRAME_HAVE_CLIP_SYNC
    return tr("Gyro sync: camera timing (fine-tune needs a build with OpenCV).");
#else
    return QString();
#endif
}
