#include "SmokeTest.h"

#include "MainWindow.h"
#include "TimelineWidget.h"
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
#include "PlateJobController.h"
#endif
#ifdef ORBIS_REFRAME_HAVE_TRACKING
#include "SubjectTrackController.h"
#endif

#include "../core/Project.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QShortcut>
#include <QTimer>

#include <cmath>
#include <cstdio>

SmokeTest::SmokeTest(MainWindow *window, const QString &scriptPath, QObject *parent)
    : QObject(parent), m_window(window), m_timer(new QTimer(this))
{
    // Qt's own file dialog, whose name field the save-as step types into.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);

    QFile f(scriptPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result(QStringLiteral("script"), QStringLiteral("unreadable"));
    } else {
        for (const QString &raw : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
            const QString line = raw.section(QLatin1Char('#'), 0, 0).trimmed();
            if (!line.isEmpty())
                m_steps.append(line);
        }
    }

    // The jobs are connected to now, so a run that ends before its wait step
    // starts isn't missed. MainWindow's own handlers were connected first and
    // run first: by the time a wait step sees the end, the results are on
    // the clip.
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    if (auto *tracker = window->findChild<SubjectTrackController *>())
        connect(tracker, &SubjectTrackController::finished, this,
                [this](const subjecttrack::Result &r) {
                    ++m_track.ended;
                    m_track.count = int(r.samples.size());
                    m_track.error = QString::fromStdString(r.error);
                });
#endif
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    if (auto *plates = window->findChild<PlateJobController *>())
        connect(plates, &PlateJobController::scanFinished, this,
                [this](std::shared_ptr<const platescan::Result> r) {
                    ++m_scan.ended;
                    m_scan.count = r ? int(r->scan.frames.size()) : 0;
                    m_scan.plates = r ? r->scan.plates : 0;
                    m_scan.error = r ? QString::fromStdString(r->error) : QStringLiteral("no result");
                });
#endif

    m_sinceStart.start();
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, &SmokeTest::tick);
    m_timer->start();
    next();
}

void SmokeTest::result(const QString &name, const QString &value)
{
    std::printf("[smoke] %s=%s\n", qPrintable(name), qPrintable(value));
    std::fflush(stdout);
}

void SmokeTest::next()
{
    ++m_step;
    m_started = false;
    m_stepClock.start();
}

void SmokeTest::fail(const QString &why)
{
    const QStringList args = m_steps.at(m_step).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    // Steps that measure something record the failure under their name.
    const QString name = args.size() > 1 ? args.at(1) : args.at(0);
    result(name, why);
    next();
}

bool SmokeTest::timedOut(int secondsArg) const
{
    const QStringList args = m_steps.at(m_step).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const double s = args.size() > secondsArg ? args.at(secondsArg).toDouble() : 10.0;
    return m_stepClock.elapsed() > s * 1000.0;
}

QWidget *SmokeTest::modalTitled(const QString &pattern) const
{
    QWidget *modal = QApplication::activeModalWidget();
    if (!modal)
        return nullptr;
    const QRegularExpression re(QRegularExpression::anchoredPattern(pattern));
    return re.match(modal->windowTitle()).hasMatch() ? modal : nullptr;
}

void SmokeTest::tick()
{
    if (m_step >= m_steps.size()) {
        m_timer->stop();
        result(QStringLiteral("script"), QStringLiteral("ended without quit"));
        if (auto *quit = m_window->findChild<QAction *>(QStringLiteral("quitAction")))
            QMetaObject::invokeMethod(quit, &QAction::trigger, Qt::QueuedConnection);
        else
            QApplication::quit();
        return;
    }
    const QStringList args = m_steps.at(m_step).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString &cmd = args.at(0);
    const auto arg = [&args](int i) { return i < args.size() ? args.at(i) : QString(); };
    const bool start = !m_started;
    m_started = true;
    EquirectViewportWindow *viewport = m_window->viewport();
    auto *timelineWidget = m_window->findChild<TimelineWidget *>();
    auto *project = m_window->findChild<Project *>();
    Timeline *timeline = project ? project->timeline() : nullptr;

    if (cmd == QLatin1String("wait-frame") || cmd == QLatin1String("wait-drawn")) {
        const bool drawn = cmd == QLatin1String("wait-drawn");
        if (viewport && (drawn ? viewport->FramesTaken() : viewport->FramesHanded()) > 0) {
            result(arg(1), QString::number(m_sinceStart.elapsed()));
            next();
        } else if (timedOut(2)) {
            fail(QStringLiteral("timeout"));
        }
    } else if (cmd == QLatin1String("key")) {
        const QKeySequence seq = QKeySequence::fromString(arg(1), QKeySequence::PortableText);
        for (QShortcut *sc : m_window->findChildren<QShortcut *>())
            if (sc->key() == seq && sc->isEnabled()) {
                // Queued, like a key press: a step that opens a dialog
                // returns before the dialog's own event loop runs.
                QMetaObject::invokeMethod(sc, [sc] { emit sc->activated(); }, Qt::QueuedConnection);
                next();
                return;
            }
        fail(QStringLiteral("no shortcut ") + arg(1));
    } else if (cmd == QLatin1String("trigger")) {
        auto *a = m_window->findChild<QAction *>(arg(1));
        if (!a || !a->isEnabled()) {
            fail(a ? QStringLiteral("disabled ") + arg(1) : QStringLiteral("no action ") + arg(1));
            return;
        }
        QMetaObject::invokeMethod(a, &QAction::trigger, Qt::QueuedConnection);
        next();
    } else if (cmd == QLatin1String("click")) {
        auto *b = m_window->findChild<QAbstractButton *>(arg(1));
        if (!b || !b->isEnabled()) {
            fail(b ? QStringLiteral("disabled ") + arg(1) : QStringLiteral("no button ") + arg(1));
            return;
        }
        QMetaObject::invokeMethod(b, &QAbstractButton::click, Qt::QueuedConnection);
        next();
    } else if (cmd == QLatin1String("sleep")) {
        if (m_stepClock.elapsed() >= arg(1).toInt())
            next();
    } else if (cmd == QLatin1String("play")) {
        auto *space = [this] {
            const QKeySequence seq(Qt::Key_Space);
            for (QShortcut *sc : m_window->findChildren<QShortcut *>())
                if (sc->key() == seq)
                    return sc;
            return static_cast<QShortcut *>(nullptr);
        }();
        if (!space || !viewport || !timelineWidget) {
            fail(QStringLiteral("no play shortcut"));
            return;
        }
        if (start) {
            m_handedAtStart = viewport->FramesHanded();
            m_takenAtStart = viewport->FramesTaken();
            m_playheadAtStart = timelineWidget->playhead();
            emit space->activated();
        } else if (m_stepClock.elapsed() >= arg(2).toInt()) {
            emit space->activated();
            result(arg(1) + QStringLiteral("_decoded"),
                   QString::number(viewport->FramesHanded() - m_handedAtStart));
            result(arg(1) + QStringLiteral("_drawn"),
                   QString::number(viewport->FramesTaken() - m_takenAtStart));
            result(arg(1) + QStringLiteral("_advance_ms"),
                   QString::number(timelineWidget->playhead() - m_playheadAtStart));
            next();
        }
    } else if (cmd == QLatin1String("grab")) {
        if (!viewport || !viewport->supportsGrab()) {
            fail(QStringLiteral("no grab"));
            return;
        }
        const QImage img = viewport->grab().convertToFormat(QImage::Format_RGB32);
        if (img.isNull()) {
            fail(QStringLiteral("empty grab"));
            return;
        }
        img.save(arg(2));
        // Rec. 601 luma, every 4th pixel each way.
        double sum = 0, sum2 = 0;
        qint64 n = 0;
        for (int y = 0; y < img.height(); y += 4) {
            const auto *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); x += 4) {
                const double l = 0.299 * qRed(row[x]) + 0.587 * qGreen(row[x]) + 0.114 * qBlue(row[x]);
                sum += l;
                sum2 += l * l;
                ++n;
            }
        }
        const double mean = n ? sum / n : 0.0;
        const double sd = n ? std::sqrt(std::max(0.0, sum2 / n - mean * mean)) : 0.0;
        // Where the viewport is in the window, in logical pixels.
        if (auto *holder = m_window->findChild<QWidget *>(QStringLiteral("viewport"))) {
            const QPoint at = holder->mapTo(m_window, QPoint(0, 0));
            result(arg(1) + QStringLiteral("_rect"), QStringLiteral("%1,%2 %3x%4")
                                                         .arg(at.x())
                                                         .arg(at.y())
                                                         .arg(holder->width())
                                                         .arg(holder->height()));
        }
        result(arg(1) + QStringLiteral("_size"),
               QStringLiteral("%1x%2").arg(img.width()).arg(img.height()));
        result(arg(1) + QStringLiteral("_luma_mean"), QString::number(mean, 'f', 1));
        result(arg(1) + QStringLiteral("_luma_stddev"), QString::number(sd, 'f', 1));
        next();
    } else if (cmd == QLatin1String("box")) {
        if (!viewport) {
            fail(QStringLiteral("no viewport"));
            return;
        }
        const double x0 = arg(1).toDouble(), y0 = arg(2).toDouble();
        const double x1 = arg(3).toDouble(), y1 = arg(4).toDouble();
        emit viewport->boxSelected(QRectF(QPointF(x0, y0), QPointF(x1, y1)));
        next();
    } else if (cmd == QLatin1String("wait-track") || cmd == QLatin1String("wait-scan")) {
        const bool track = cmd == QLatin1String("wait-track");
        JobEnd &job = track ? m_track : m_scan;
        if (job.ended > job.taken) {
            ++job.taken;
            result(arg(1) + (track ? QStringLiteral("_samples") : QStringLiteral("_frames")),
                   QString::number(job.count));
            if (track) {
                int keys = 0;
                for (int i = 0; timeline && i < timeline->count(); ++i)
                    keys += timeline->clipAt(i).reframe.count();
                result(arg(1) + QStringLiteral("_keys"), QString::number(keys));
            } else {
                result(arg(1) + QStringLiteral("_plates"), QString::number(job.plates));
            }
            result(arg(1) + QStringLiteral("_error"), job.error);
            next();
        } else if (timedOut(2)) {
            fail(QStringLiteral("timeout"));
        }
    } else if (cmd == QLatin1String("dialog")) {
        // The title may contain spaces: everything between the name and the
        // seconds.
        const QString title = args.mid(2, args.size() - 3).join(QLatin1Char(' '));
        if (QWidget *w = modalTitled(title)) {
            result(arg(1), w->windowTitle());
            if (auto *box = qobject_cast<QMessageBox *>(w)) {
                for (QAbstractButton *b : box->buttons()) {
                    const auto role = box->buttonRole(b);
                    if (role == QMessageBox::AcceptRole || role == QMessageBox::YesRole) {
                        b->click();
                        next();
                        return;
                    }
                }
            }
            if (auto *d = qobject_cast<QDialog *>(w))
                d->accept();
            next();
        } else if (m_stepClock.elapsed() > args.last().toDouble() * 1000.0) {
            QWidget *other = QApplication::activeModalWidget();
            fail(other ? QStringLiteral("timeout (up: %1)").arg(other->windowTitle())
                       : QStringLiteral("timeout"));
        }
    } else if (cmd == QLatin1String("save-as")) {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        if (dialog) {
            // Typed into the name field: selectFile() is lost while the
            // dialog's directory listing is still loading.
            if (auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
                name->setText(arg(3));
            result(arg(1), dialog->windowTitle());
            static_cast<QDialog *>(dialog)->accept(); // QFileDialog's own is protected
            next();
        } else if (timedOut(2)) {
            fail(QStringLiteral("timeout"));
        }
    } else if (cmd == QLatin1String("timeline")) {
        result(arg(1) + QStringLiteral("_ms"),
               QString::number(timeline ? timeline->totalDurationMs() : -1));
        next();
    } else if (cmd == QLatin1String("quit")) {
        m_timer->stop();
        result(QStringLiteral("script"), QStringLiteral("done"));
        if (auto *quit = m_window->findChild<QAction *>(QStringLiteral("quitAction")))
            QMetaObject::invokeMethod(quit, &QAction::trigger, Qt::QueuedConnection);
        else
            QApplication::quit();
    } else {
        fail(QStringLiteral("unknown step ") + cmd);
    }
}
