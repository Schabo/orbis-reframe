#include "ExportRunner.h"

#include <QImage>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>
#include <QWidget>

#include <InsetCompositor.h> // exportpipe::InsetCompositor

ExportRunner::ExportRunner(QWidget *window) : QObject(window), m_window(window) {}

ExportRunner::~ExportRunner()
{
    if (m_stillThread)
        m_stillThread->wait();
}

void ExportRunner::startExport(const ReframeExportController::Request &req)
{
    if (m_exporting)
        return;
    auto *thread = new QThread(this);
    auto *worker = new ReframeExportController;
    worker->moveToThread(thread);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);

    auto *dlg = new QProgressDialog(tr("Preparing…"), tr("Cancel"), 0, 100, m_window);
    dlg->setWindowTitle(tr("Exporting flat video"));
    dlg->setWindowModality(Qt::ApplicationModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setValue(0);

    m_exporting = true;
    emit busyChanged();

    // Cancel: flip the worker's atomic flag (thread-safe -- a lone atomic
    // store, so calling it straight from the GUI thread is fine), and hold the
    // dialog up with a "Cancelling…" note until the worker stops and finished()
    // closes it. The worker deletes the partial output file itself. Context is
    // `dlg` (GUI thread), NOT `worker` -- the worker thread is blocked in run().
    connect(dlg, &QProgressDialog::canceled, dlg, [worker, dlg] {
        worker->cancel();
        dlg->setLabelText(tr("Cancelling…"));
        dlg->show();
    });

    connect(worker, &ReframeExportController::progress, dlg, [dlg](int pct, const QString &label) {
        if (dlg->wasCanceled())
            return;
        if (!label.isEmpty())
            dlg->setLabelText(label);
        dlg->setValue(pct);
    });
    connect(worker, &ReframeExportController::finished, this,
            [this, dlg, thread](bool ok, const QString &error, const QString &outPath) {
                dlg->close();
                dlg->deleteLater();
                thread->quit();
                thread->wait();
                thread->deleteLater();
                m_exporting = false;
                emit busyChanged();
                if (ok)
                    QMessageBox::information(m_window, tr("Export complete"),
                                             tr("Wrote %1").arg(outPath));
                else if (!error.isEmpty())
                    QMessageBox::warning(m_window, tr("Export failed"), error);
                // else: cancelled by the user -- the dialog just closes, and
                // ReframeExportController has already deleted the partial file.
            });

    thread->start();
    QMetaObject::invokeMethod(worker, [worker, req] { worker->run(req); });
}

void ExportRunner::startStill(Still still)
{
    if (m_stillThread)
        return;
    m_stillThread = QThread::create([this, s = std::move(still)] {
        QString err;
        QImage img = ReframeExportController::renderStill(s.job, s.sourceMs, s.pose.data(), s.width,
                                                          s.height, s.lensGuard, s.depthSeam, err);
        if (!img.isNull() && !s.insets.empty()) {
            exportpipe::InsetCompositor comp(s.insets, s.width, s.height);
            std::string e;
            if (!comp.compositeRgb(s.timelineMs, img.bits(), int(img.bytesPerLine()), 3, e))
                err = QString::fromStdString(e);
        }
        if (img.isNull() && err.isEmpty())
            err = tr("The frame couldn't be rendered.");
        else if (!img.isNull() && err.isEmpty() && !img.save(s.path, "PNG"))
            err = tr("Couldn't write %1").arg(s.path);
        QMetaObject::invokeMethod(this, [this, err, path = s.path] {
            m_stillThread->wait(); // its function has just returned
            m_stillThread->deleteLater();
            m_stillThread = nullptr;
            emit busyChanged();
            emit stillFinished(path, err);
        }, Qt::QueuedConnection);
    });
    m_stillThread->start();
    emit busyChanged();
}
