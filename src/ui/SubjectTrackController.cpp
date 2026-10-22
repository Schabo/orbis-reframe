#include "SubjectTrackController.h"

#include "../render/EquirectViewportWindow.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QThread>
#include <QWidget>
#include <QWindow>

#include <cmath>
#include <functional>
#include <memory>
#include <numbers>

namespace {

// While Track subject plays back what it tracks, every click and key goes
// nowhere -- no seeking, editing or menus underneath the run -- except the
// Reframe panel's Stop button, and Space / Esc / T, which stop it. Installed
// on the application, so it sees the embedded viewport window's events and
// the ShortcutOverride that comes before an application shortcut fires.
class TrackingInputLock : public QObject {
public:
    TrackingInputLock(QWidget *allowed, QWindow *viewport, std::function<void()> stop,
                      QObject *parent)
        : QObject(parent), m_allowed(allowed), m_viewport(viewport), m_stop(std::move(stop))
    {
    }

    bool eventFilter(QObject *obj, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::ShortcutOverride:
            e->accept(); // "the focus wants this key" -> no shortcut fires
            return true;
        case QEvent::KeyPress: {
            const auto *k = static_cast<QKeyEvent *>(e);
            if (!k->isAutoRepeat() && (k->key() == Qt::Key_Space || k->key() == Qt::Key_Escape ||
                                       k->key() == Qt::Key_T))
                m_stop();
            return true;
        }
        case QEvent::KeyRelease:
            return true;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::ContextMenu:
            if (obj == m_viewport)
                return true;
            if (auto *w = qobject_cast<QWidget *>(obj))
                return !(m_allowed && (w == m_allowed || m_allowed->isAncestorOf(w)));
            return false; // top-level windows pass mouse events on to their widgets
        default:
            return false;
        }
    }

private:
    QPointer<QWidget> m_allowed;
    QWindow *m_viewport;
    std::function<void()> m_stop;
};

} // namespace

SubjectTrackController::SubjectTrackController(EquirectViewportWindow *viewport,
                                               QWidget *stopButton, QObject *parent)
    : QObject(parent), m_viewport(viewport), m_stopButton(stopButton)
{
}

SubjectTrackController::~SubjectTrackController()
{
    if (m_thread) {
        m_stop = true;
        m_thread->wait(); // stops within a frame
        delete m_thread;
    }
    if (m_inputLock)
        qApp->removeEventFilter(m_inputLock);
}

bool SubjectTrackController::start(const Run &run)
{
    if (m_thread)
        return false;
    m_run = run;
    m_stop = false;
    m_inputLock = new TrackingInputLock(m_stopButton, m_viewport, [this] { m_stop = true; }, this);
    qApp->installEventFilter(m_inputLock);

    // Per tracked frame, on the worker thread: the camera eases toward the
    // subject (the saved keys are smoothed both ways afterwards; this is a
    // causal stand-in) and the box is drawn where the tracker has it, green
    // while verified, red while lost. Picture, pose and box go to the
    // viewport together; progress() goes out on the GUI thread.
    struct Live {
        double camYaw, camPitch;
        int frames = 0;
        qint64 lostSinceMs = -1;
    };
    const ReframePose look = run.look;
    const float aspect = run.aspect;
    const double fps = run.req.fps;
    auto live = std::make_shared<Live>(Live{look.yaw, look.pitch});
    auto onFrame = [this, live, look, aspect, fps](stitch::Frame &&f, const float stab[9],
                                                  const track::TrackSample &s) {
        constexpr double kPi = std::numbers::pi;
        const double a = 1.0 - std::exp(-1.0 / (fps * 0.25)); // ~0.25 s time constant
        double dy = std::remainder(s.yaw - live->camYaw, 2.0 * kPi);
        live->camYaw += a * dy;
        live->camPitch += a * (s.pitch - live->camPitch);
        EquirectViewportWindow::FrameOverlay o;
        o.pose = look;
        o.pose.yaw = float(live->camYaw);
        o.pose.pitch = float(live->camPitch);
        const track::Vec3 v = track::stabToView(track::dirFromYawPitch(s.yaw, s.pitch),
                                                live->camYaw, live->camPitch, look.roll);
        const view::Lens lens = view::lens(look, aspect);
        const double d = lens.distortion;
        const double p[3] = {v.x, v.y, v.z};
        double px = 0.0, py = 0.0;
        if (view::pointToPlane(p, d, px, py, 1e-6)) {
            // The box at the subject's angular size, as the view's centre shows it.
            const double t = view::halfHeight<double>(lens.fovY, d);
            const double fx = (px / (t * aspect) + 1.0) * 0.5;
            const double fy = (-py / t + 1.0) * 0.5;
            const double fw = view::planeRadius(s.width * 0.5, d) / (t * aspect);
            const double fh = view::planeRadius(s.height * 0.5, d) / t;
            o.hasMarker = true;
            o.marker = QRectF(fx - fw * 0.5, fy - fh * 0.5, fw, fh);
            o.markerStyle = s.good ? EquirectViewportWindow::MarkerStyle::Tracking
                                   : EquirectViewportWindow::MarkerStyle::Lost;
        }
        const int64_t ms = f.timestamp_ms;
        m_viewport->SetFrame(f.width, f.height, std::move(f.rgba), stab, o);

        if (s.good)
            live->lostSinceMs = -1;
        else if (live->lostSinceMs < 0)
            live->lostSinceMs = ms;
        const qint64 lostFor = live->lostSinceMs < 0 ? -1 : ms - live->lostSinceMs;
        if ((++live->frames % 3) == 0 || lostFor == 0)
            QMetaObject::invokeMethod(this, [this, ms, lostFor] {
                if (m_thread)
                    emit progress(ms, lostFor);
            }, Qt::QueuedConnection);
    };

    const subjecttrack::Request req = run.req;
    m_thread = QThread::create([this, req, onFrame] {
        const subjecttrack::Result r = subjecttrack::run(req, m_stop, onFrame);
        QMetaObject::invokeMethod(this, [this, r] {
            m_thread->wait(); // its function has just returned
            m_thread->deleteLater();
            m_thread = nullptr;
            qApp->removeEventFilter(m_inputLock);
            m_inputLock->deleteLater();
            m_inputLock = nullptr;
            m_viewport->EndFrameOverlays();
            emit finished(r);
        }, Qt::QueuedConnection);
    });
    m_thread->start();
    return true;
}
