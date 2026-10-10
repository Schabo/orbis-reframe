#include "PlateJobController.h"

#include <QThread>

PlateJobController::PlateJobController(QObject *parent) : QObject(parent) {}

PlateJobController::~PlateJobController()
{
    if (m_thread) {
        m_stop = true;
        m_thread->wait(); // stops within a frame
        delete m_thread;
    }
}

template <typename Job, typename Done>
void PlateJobController::launch(Job job, Done done)
{
    m_stop = false;
    m_thread = QThread::create([this, job, done] {
        auto r = job();
        QMetaObject::invokeMethod(this, [this, r, done] {
            m_thread->wait(); // its function has just returned
            m_thread->deleteLater();
            m_thread = nullptr;
            done(r);
        }, Qt::QueuedConnection);
    });
    m_thread->start();
}

bool PlateJobController::startScan(const platescan::Request &req)
{
    if (m_thread)
        return false;
    m_following = false;
    auto progress = [this](int64_t tMs, double fps) {
        QMetaObject::invokeMethod(this, [this, tMs, fps] {
            if (m_thread)
                emit scanProgress(tMs, fps);
        }, Qt::QueuedConnection);
    };
    launch(
        [this, req, progress] {
            return std::make_shared<const platescan::Result>(platescan::run(req, m_stop, progress));
        },
        [this](std::shared_ptr<const platescan::Result> r) { emit scanFinished(r); });
    return true;
}

bool PlateJobController::startFollow(const platescan::Request &req, const insv::BlurQuad &seed,
                                     qint64 backToMs)
{
    if (m_thread)
        return false;
    m_following = true;
    auto progress = [this](int64_t tMs, bool backward) {
        QMetaObject::invokeMethod(this, [this, tMs, backward] {
            if (m_thread)
                emit followProgress(tMs, backward);
        }, Qt::QueuedConnection);
    };
    launch(
        [this, req, seed, backToMs, progress] {
            return std::make_shared<const platescan::FollowResult>(
                platescan::follow(req, seed, backToMs, m_stop, progress));
        },
        [this](std::shared_ptr<const platescan::FollowResult> r) { emit followFinished(r); });
    return true;
}
