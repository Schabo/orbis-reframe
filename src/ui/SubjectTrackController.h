#pragma once

#include <QObject>
#include <QPointer>

#include <atomic>

#include "SubjectTrackJob.h"       // subjecttrack::Request, Result
#include "../core/ReframeTrack.h" // ReframePose

class QThread;
class QWidget;
class EquirectViewportWindow;

// Runs "Track subject" (subjecttrack::run) on a worker thread and plays what
// it tracks back in the viewport as it goes: the camera easing after the
// subject, its box drawn over the picture (green while verified, red while
// lost). While it runs every click and key goes nowhere except `stopButton`
// and Space / Esc / T, which stop it.
//
// MainWindow builds the request from the box drawn in the viewport, and on
// finished() writes the reduced path into the clip's ReframeTrack -- this
// class never touches the edit model. Only built with subject tracking
// (ORBIS_REFRAME_HAVE_TRACKING).
class SubjectTrackController : public QObject {
    Q_OBJECT

public:
    // One tracking run: what MainWindow started it with, for its progress
    // and finished() handlers.
    struct Run {
        subjecttrack::Request req;
        int clipId = 0;
        int pane = 0;        // the split clip's pane whose camera it writes
        ReframePose look;    // the view the box was drawn in
        float aspect = 1.0f; // that view's aspect (the active pane's)
    };

    SubjectTrackController(EquirectViewportWindow *viewport, QWidget *stopButton,
                           QObject *parent = nullptr);
    // Stops a run and waits for it (within a frame).
    ~SubjectTrackController() override;

    bool running() const { return m_thread != nullptr; }
    // The current run, or the one that just finished (in finished()).
    const Run &run() const { return m_run; }
    // False if a run is already going.
    bool start(const Run &run);
    void stop() { m_stop = true; }

signals:
    // On the GUI thread, every third frame and when the subject is lost:
    // the source time reached, and how long the subject has been lost
    // (-1 = it hasn't).
    void progress(qint64 sourceMs, qint64 lostForMs);
    // The run is over (stopped, ended, lost or failed); the viewport is back
    // to normal frames.
    void finished(const subjecttrack::Result &result);

private:
    EquirectViewportWindow *m_viewport;
    QPointer<QWidget> m_stopButton;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_stop{false};
    QObject *m_inputLock = nullptr;
    Run m_run;
};
