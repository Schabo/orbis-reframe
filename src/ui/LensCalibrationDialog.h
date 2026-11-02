#pragma once

#include <IStitchSource.h> // stitch::PreviewStitchConfig::LensGuard, LensCalibration

#include <QDialog>

#include <atomic>
#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QThread;
class ChartWindow;

// Calibrate lenses: measures the user's own camera with the chart (src/calib)
// in four steps -- show the chart on a monitor (with the size of its
// squares), film it, pick the recording and let it be measured, then the
// result, kept (lenscal::Save) only when it passes. The measuring runs on a
// worker thread; closing the dialog stops it.
class LensCalibrationDialog : public QDialog {
    Q_OBJECT

public:
    using LensGuard = stitch::PreviewStitchConfig::LensGuard;

    // `startDir`: where the recording is looked for first; `guard`: the
    // lens guards to start with (the shown clip's).
    LensCalibrationDialog(const QString &startDir, LensGuard guard, QWidget *parent = nullptr);
    ~LensCalibrationDialog() override;

signals:
    // A calibration that passed, kept for its camera and guard;
    // `useInProject`: put it on the project's clips of that camera and guard.
    void calibrationSaved(const stitch::LensCalibration &c, bool useInProject);

protected:
    void reject() override;

private:
    enum Page { ChartPage, FilmPage, MeasurePage, ResultPage };
    void showPage(int page);
    void updateButtons();
    void screenChosen();
    void showChart();
    void browse();
    void start();
    void stop();
    void finished(bool ok, const QString &message);
    void save();

    QStackedWidget *m_pages = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_next = nullptr;
    QPushButton *m_close = nullptr;

    QComboBox *m_screen = nullptr;
    QDoubleSpinBox *m_square = nullptr;
    QLabel *m_squareHint = nullptr;
    QPushButton *m_showChart = nullptr;
    ChartWindow *m_chart = nullptr;

    QLineEdit *m_recording = nullptr;
    QComboBox *m_guard = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;

    QLabel *m_result = nullptr;
    QCheckBox *m_useInProject = nullptr;
    QPushButton *m_save = nullptr;

    QThread *m_worker = nullptr;
    std::atomic<bool> m_cancel{false};
    std::optional<stitch::LensCalibration> m_calibration; // the last one measured
    bool m_passed = false;
};
