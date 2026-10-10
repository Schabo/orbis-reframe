#pragma once

#include <QObject>

#include <atomic>
#include <memory>

#include "PlateScanJob.h" // platescan::Request, Result, FollowResult

class QThread;

// Runs the privacy blur's background jobs -- "Scan clip" (platescan::run) and
// "Blur a plate…" (platescan::follow) -- on a worker thread, one at a time:
// both stitch the clip and run the detector on the GPU. Progress and the
// result come back on the GUI thread as signals; MainWindow builds the
// requests and puts the results on the clip (the scan cache, the clip's
// blur additions) -- this class never touches the edit model. Only built
// with the plate scan (ORBIS_REFRAME_HAVE_PLATE_SCAN).
class PlateJobController : public QObject {
    Q_OBJECT

public:
    explicit PlateJobController(QObject *parent = nullptr);
    // Stops a running job and waits for it (within a frame).
    ~PlateJobController() override;

    bool running() const { return m_thread != nullptr; }
    // The running job is a follow ("Blur a plate…"), not a scan.
    bool following() const { return m_thread && m_following; }

    // Both return false when a job is already running.
    bool startScan(const platescan::Request &req);
    // Follow the plate `seed` from req.fromMs to req.toMs, then back from
    // req.fromMs to `backToMs`.
    bool startFollow(const platescan::Request &req, const insv::BlurQuad &seed, qint64 backToMs);
    // A stopped scan keeps the part scanned; a stopped follow, the part
    // followed.
    void stop() { m_stop = true; }

signals:
    void scanProgress(qint64 tMs, double fps);
    void scanFinished(std::shared_ptr<const platescan::Result> result);
    void followProgress(qint64 tMs, bool backward);
    void followFinished(std::shared_ptr<const platescan::FollowResult> result);

private:
    // Start `job` on m_thread; `done` runs on the GUI thread after it returns.
    template <typename Job, typename Done>
    void launch(Job job, Done done);

    QThread *m_thread = nullptr;
    bool m_following = false;
    std::atomic<bool> m_stop{false};
};
