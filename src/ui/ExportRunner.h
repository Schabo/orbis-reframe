#pragma once

#include <QObject>
#include <QString>

#include <array>
#include <vector>

#include "ReframeExportController.h" // Request, ClipJob

class QThread;
class QWidget;

// Runs the renders that write a file, off the GUI thread: File ▸ Export flat
// video (a ReframeExportController on a scratch thread, behind a modal
// progress dialog with Cancel, then a message saying it's done or why it
// failed) and a screenshot (one frame through the export path, saved as a
// PNG). One of each at a time; MainWindow builds the requests from the edit
// model and asks the user where to write.
class ExportRunner : public QObject {
    Q_OBJECT

public:
    // `window`: the parent of the progress dialog and the messages.
    explicit ExportRunner(QWidget *window);
    // Waits for a screenshot being saved (a frame: a second or two at most).
    ~ExportRunner() override;

    bool exporting() const { return m_exporting; }
    bool savingStill() const { return m_stillThread != nullptr; }

    // Export `req` (the caller paused playback). No-op while one runs.
    void startExport(const ReframeExportController::Request &req);

    // A screenshot: clip `job`'s frame at `sourceMs` looked at with `pose`
    // {yaw, pitch, roll, fovY, proj}, at w x h, with `insets` composited at
    // `timelineMs` like the export does; written to `path` as a PNG. No-op
    // while one is being saved.
    struct Still {
        ReframeExportController::ClipJob job;
        std::array<float, 5> pose{};
        qint64 sourceMs = 0;
        qint64 timelineMs = 0;
        int width = 0, height = 0;
        std::vector<exportpipe::InsetClipJob> insets;
        QString path;
    };
    void startStill(Still still);

signals:
    // An export or a screenshot started or finished.
    void busyChanged();
    // The screenshot is written (`error` empty) or failed.
    void stillFinished(const QString &path, const QString &error);

private:
    QWidget *m_window;
    bool m_exporting = false;
    QThread *m_stillThread = nullptr;
};
