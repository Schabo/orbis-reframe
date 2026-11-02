#include "ClipMeasurements.h"

#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>

#include <ClipLensColour.h> // stitch::MeasureClipLensColour
#include <ClipSeamAnalysis.h> // stitch::AnalyseClipSeam
#include <ClipSeam.h>       // stitch::MeasureClipSeam
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
    if (m_colourThread) {
        m_colourCancel = true;
        m_colourThread->wait();
    }
    if (m_analysisThread) {
        m_analysisCancel = true;
        m_analysisThread->wait();
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

// The seam fit and the lens colour are the old stitcher's; the new one joins
// the lenses from their calibration alone.
bool ClipMeasurements::usesOldStitcher(const ClipItem &c)
{
    return c.stitchMethod != stitch::PreviewStitchConfig::Method::New;
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
        QStringList paths; // the playhead's source first
        for (int i = 0; i < t->count(); ++i)
            if (!t->clipAt(i).flat && !paths.contains(t->clipAt(i).sourcePath))
                paths << t->clipAt(i).sourcePath;
        if (paths.removeOne(m_priorityPath))
            paths.prepend(m_priorityPath);
        for (const QString &path : std::as_const(paths)) {
            maybeMeasureSync(path);
            maybeMeasureSeam(path);
            maybeMeasureColour(path);
        }
        for (const QString &path : std::as_const(paths))
            maybeAnalyseSeam(path); // after any measurement it depends on
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
        m_colourCancel = true;
        m_analysisCancel = true; // keeps what it has done
        return;
    }
    schedule(); // measurements held back while playing
}

void ClipMeasurements::setPriorityPath(const QString &clipPath)
{
    if (clipPath == m_priorityPath)
        return;
    m_priorityPath = clipPath;
    schedule();
}

void ClipMeasurements::maybeAnalyseSeam(const QString &clipPath)
{
    if (!m_enabled || clipPath.isEmpty() || m_analysisThread || m_seamThread || m_colourThread ||
        m_analysisFailed.contains(clipPath))
        return;
    // After the seam and lens colour measurements: the seam's state depends
    // on both, as the stitch has them (none, if one failed).
    const Timeline *t = m_project->timeline();
    insv::SeamCorrection seam;
    insv::LensColour colour;
    if (!(t->sourceSeamCorrection(clipPath, seam) || m_seamFailed.contains(clipPath)) ||
        !(t->sourceLensColour(clipPath, colour) || m_colourFailed.contains(clipPath)))
        return;
    const ClipItem *clip = measurableClip(clipPath);
    if (!clip)
        return;
    // The stretches of the source on the timeline with the depth-aware seam
    // on, under the same lens guard (another guard is another stitch).
    std::vector<std::pair<qint64, qint64>> ranges;
    for (int i = 0; i < t->count(); ++i) {
        const ClipItem &c = t->clipAt(i);
        if (c.sourcePath == clipPath &&
            c.stitchMethod == stitch::PreviewStitchConfig::Method::DepthSeam && !c.flat &&
            !c.mediaMissing && c.lensGuard == clip->lensGuard &&
            c.lensCalibration == clip->lensCalibration)
            ranges.emplace_back(c.trimInMs, c.trimOutMs);
    }
    if (ranges.empty())
        return;
    const stitch::SeamAnalysisKey key{clipPath.toStdString(), clip->lensGuard, seam, colour,
                                      clip->lensCalibration};
    const std::shared_ptr<stitch::SeamAnalysis> analysis = stitch::SeamAnalysisFor(key);
    qint64 total = 0, done = 0;
    for (const auto &[in, out] : ranges) {
        total += out - in;
        done += std::llround(analysis->coverage(in, out) * (out - in));
    }
    if (std::all_of(ranges.begin(), ranges.end(),
                    [&](const auto &r) { return analysis->covers(r.first, r.second); }))
        return;
    m_analysisPath = clipPath;
    m_analysisCancel = false;
    m_analysisPermille = static_cast<int>(total > 0 ? 1000 * done / total : 0);
    m_analysisThread = QThread::create([this, key, ranges, total, clipPath] {
        stitch::SeamAnalysisResult r;
        r.ok = true;
        qint64 before = 0; // the length of the ranges already gone through
        for (const auto &[in, out] : ranges) {
            const qint64 len = out - in;
            auto progress = [this, before, len, total](double share) {
                m_analysisPermille =
                    static_cast<int>(total > 0 ? 1000.0 * (before + share * len) / total : 0.0);
                QMetaObject::invokeMethod(this, [this] { emit statusChanged(); }, Qt::QueuedConnection);
            };
            r = stitch::AnalyseClipSeam(key, in, out, &m_analysisCancel, progress, true);
            before += len;
            if (!r.ok)
                break;
        }
        QMetaObject::invokeMethod(this, [this, clipPath, r] {
            m_analysisThread->wait(); // its function has just returned
            m_analysisThread->deleteLater();
            m_analysisThread = nullptr;
            m_analysisPath.clear();
            core::log::Info("[seam analysis] %s: %s\n", qPrintable(clipPath), r.message.c_str());
            if (!r.ok && !r.cancelled)
                m_analysisFailed.insert(clipPath, QString::fromStdString(r.message));
            emit seamAnalysed(clipPath);
            emit statusChanged();
            schedule(); // another source may be waiting for its turn
        }, Qt::QueuedConnection);
    });
    m_analysisThread->start(QThread::LowPriority);
    core::log::Info("[seam analysis] %s: analysing\n", qPrintable(clipPath));
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
    const auto cal = clip->lensCalibration;
    m_syncThread = QThread::create([this, path, in, out, clipPath, guard, cal] {
        const stitch::ClipSyncResult r =
            stitch::MeasureClipSync(path, in, out, &m_syncCancel, guard, cal ? &*cal : nullptr);
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
    if (!clip || !usesOldStitcher(*clip))
        return;
    const std::string path = clipPath.toStdString();
    const qint64 in = clip->trimInMs, out = clip->trimOutMs;
    const auto guard = clip->lensGuard;
    const auto cal = clip->lensCalibration;
    m_seamMeasuringPath = clipPath;
    m_seamCancel = false;
    m_seamThread = QThread::create([this, path, in, out, clipPath, guard, cal] {
        const stitch::ClipSeamResult r =
            stitch::MeasureClipSeam(path, in, out, guard, cal ? &*cal : nullptr, &m_seamCancel);
        QMetaObject::invokeMethod(this, [this, clipPath, r, guard, cal] {
            m_seamThread->wait(); // its function has just returned
            m_seamThread->deleteLater();
            m_seamThread = nullptr;
            m_seamMeasuringPath.clear();
            core::log::Info("[seam] %s: %s\n", qPrintable(clipPath), r.message.c_str());
            // Stale if the guard or the calibration changed meanwhile: drop
            // it and let the re-measure below run under the new one.
            const ClipItem *now = measurableClip(clipPath);
            if (r.ok && now && guard == now->lensGuard && cal == now->lensCalibration)
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

void ClipMeasurements::maybeMeasureColour(const QString &clipPath)
{
    // After the seam: the lenses' colours are compared where they line up.
    // A source whose seam couldn't be aligned is measured with none.
    insv::LensColour colour;
    insv::SeamCorrection seam;
    if (!m_enabled || clipPath.isEmpty() || m_colourThread || m_seamThread ||
        m_project->timeline()->sourceLensColour(clipPath, colour) ||
        m_colourFailed.contains(clipPath) ||
        (!m_project->timeline()->sourceSeamCorrection(clipPath, seam) &&
         !m_seamFailed.contains(clipPath)))
        return;
    const ClipItem *clip = measurableClip(clipPath);
    if (!clip || !usesOldStitcher(*clip))
        return;
    const std::string path = clipPath.toStdString();
    const qint64 in = clip->trimInMs, out = clip->trimOutMs;
    const auto guard = clip->lensGuard;
    const auto cal = clip->lensCalibration;
    m_colourMeasuringPath = clipPath;
    m_colourCancel = false;
    m_colourThread = QThread::create([this, path, in, out, clipPath, guard, cal, seam] {
        const stitch::ClipLensColourResult r = stitch::MeasureClipLensColour(
            path, in, out, guard, cal ? &*cal : nullptr, seam, &m_colourCancel);
        QMetaObject::invokeMethod(this, [this, clipPath, r, guard, cal, seam] {
            m_colourThread->wait(); // its function has just returned
            m_colourThread->deleteLater();
            m_colourThread = nullptr;
            m_colourMeasuringPath.clear();
            core::log::Info("[colour] %s: %s\n", qPrintable(clipPath), r.message.c_str());
            // Stale if the guard, the calibration or the seam changed
            // meanwhile: dropped, and measured again below.
            const ClipItem *now = measurableClip(clipPath);
            insv::SeamCorrection seamNow;
            m_project->timeline()->sourceSeamCorrection(clipPath, seamNow);
            if (r.ok && now && guard == now->lensGuard && cal == now->lensCalibration &&
                seam == seamNow)
                m_project->timeline()->setSourceLensColour(
                    clipPath, r.colour, static_cast<int>(guard)); // -> clipSeamChanged
            else if (!r.ok && !m_colourCancel)
                m_colourFailed.insert(clipPath, r.failure);
            emit statusChanged();
            schedule(); // another source may be waiting for its turn
        }, Qt::QueuedConnection);
    });
    m_colourThread->start(QThread::LowPriority);
    core::log::Info("[colour] %s: measuring\n", qPrintable(clipPath));
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

QString ClipMeasurements::stitchStatus(const ClipItem *c, QString *details) const
{
    if (details)
        details->clear();
    if (!c)
        return QString();
    if (!usesOldStitcher(*c))
        return tr("The new stitcher measures nothing from this clip.");
    enum class State { None, Measuring, Done, Failed };
    const QString &path = c->sourcePath;
    const Timeline *t = m_project->timeline();

    insv::SeamCorrection seam;
    QString seamWhy, colourWhy;
    State s = State::None, k = State::None;
    if (t->sourceSeamCorrection(path, seam)) {
        s = State::Done;
    } else if (m_seamMeasuringPath == path) {
        s = State::Measuring;
    } else if (const auto it = m_seamFailed.constFind(path); it != m_seamFailed.cend()) {
        s = State::Failed;
        seamWhy = failureText(*it, true);
    }
    insv::LensColour colour;
    if (t->sourceLensColour(path, colour)) {
        k = State::Done;
    } else if (m_colourMeasuringPath == path) {
        k = State::Measuring;
    } else if (const auto it = m_colourFailed.constFind(path); it != m_colourFailed.cend()) {
        k = State::Failed;
        colourWhy = failureText(*it, true);
    }

    // The figures, for the tooltip: they matter when checking a clip, not
    // while editing.
    if (details) {
        QStringList lines;
        if (s == State::Done)
            lines << tr("Seam: the back lens turned by yaw %1°, pitch %2°, roll %3°; its centre "
                        "moved by %4 %, %5 %; lens scale %6 %.")
                         .arg(seam.lens1RotDeg[0], 0, 'f', 2)
                         .arg(seam.lens1RotDeg[1], 0, 'f', 2)
                         .arg(seam.lens1RotDeg[2], 0, 'f', 2)
                         .arg(seam.lens1Centre[0] * 100.0, 0, 'f', 2)
                         .arg(seam.lens1Centre[1] * 100.0, 0, 'f', 2)
                         .arg((seam.radialScale - 1.0) * 100.0, 0, 'f', 2);
        if (k == State::Done)
            lines << tr("Lens colours: the lenses differed by red %1 %, green %2 %, blue %3 %.")
                         .arg(std::expm1(colour.logRatio[0]) * 100.0, 0, 'f', 1)
                         .arg(std::expm1(colour.logRatio[1]) * 100.0, 0, 'f', 1)
                         .arg(std::expm1(colour.logRatio[2]) * 100.0, 0, 'f', 1);
        *details = lines.join(QLatin1Char('\n'));
    }

    // One short line. The colour is measured after the seam, so the seam
    // leads; a failure keeps its reason in view. The seam analysis comes
    // after both.
    if (m_analysisPath == path)
        return tr("Analysing this clip's seam… %1 %").arg(m_analysisPermille.load() / 10);
    switch (s) {
    case State::Measuring:
        return tr("Measuring this clip's seam…");
    case State::Done:
        switch (k) {
        case State::Done:
            return tr("Seam aligned and lens colours matched for this clip.");
        case State::Measuring:
            return tr("Seam aligned; measuring this clip's lens colours…");
        case State::Failed:
            return tr("Seam aligned; couldn't match the lens colours (%1).").arg(colourWhy);
        case State::None:
            break;
        }
        return tr("Seam aligned for this clip.");
    case State::Failed:
        switch (k) {
        case State::Done:
            return tr("Couldn't align this clip's seam (%1); lens colours matched.").arg(seamWhy);
        case State::Measuring:
            return tr("Couldn't align this clip's seam (%1); measuring the lens colours…")
                .arg(seamWhy);
        case State::Failed:
            return colourWhy == seamWhy
                       ? tr("Couldn't align this clip's seam or match its lens colours (%1).")
                             .arg(seamWhy)
                       : tr("Couldn't align this clip's seam (%1) or match its lens colours (%2).")
                             .arg(seamWhy, colourWhy);
        case State::None:
            break;
        }
        return tr("Couldn't align this clip's seam (%1).").arg(seamWhy);
    case State::None:
        break;
    }
    switch (k) {
    case State::Done:
        return tr("Lens colours matched for this clip.");
    case State::Measuring:
        return tr("Measuring this clip's lens colours…");
    case State::Failed:
        return tr("Couldn't match this clip's lens colours (%1).").arg(colourWhy);
    case State::None:
        break;
    }
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
