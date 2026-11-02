#include "LensCalibrationDialog.h"

#include "../core/LensCalibrationStore.h"
#include "../core/Log.h"

#include <ChartBoard.h>
#include <ChartDetect.h>
#include <ClipMetadata.h>
#include <LensFit.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QThread>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>

// The chart, full screen on one monitor, drawn pixel for pixel: the square
// size the user measures is the one the fit takes. A click or Esc closes it.
class ChartWindow : public QWidget {
public:
    explicit ChartWindow(QScreen *screen)
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint)
    {
        setObjectName(QStringLiteral("lensCalibrationChart"));
        setAttribute(Qt::WA_DeleteOnClose, false);
        setCursor(Qt::BlankCursor);
        setScreen(screen);
        setGeometry(screen->geometry());
        const qreal dpr = screen->devicePixelRatio();
        const int w = int(std::lround(screen->geometry().width() * dpr));
        const int h = int(std::lround(screen->geometry().height() * dpr));
        const calib::ChartImage chart = calib::MakeChartImage(w, h);
        m_squarePx = chart.squarePx;
        m_image = QImage(chart.grey.data(), chart.width, chart.height, chart.width,
                         QImage::Format_Grayscale8)
                      .copy();
        m_image.setDevicePixelRatio(dpr);
    }
    int squarePx() const { return m_squarePx; }
    int widthPx() const { return m_image.width(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        p.drawImage(QPointF(0, 0), m_image);
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            close();
        else
            QWidget::keyPressEvent(e);
    }
    void mousePressEvent(QMouseEvent *) override { close(); }

private:
    QImage m_image;
    int m_squarePx = 0;
};

namespace {

QLabel *Text(const QString &html, QWidget *parent)
{
    auto *l = new QLabel(html, parent);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    return l;
}

QString LensName(int lens)
{
    return lens == 0 ? LensCalibrationDialog::tr("front lens") : LensCalibrationDialog::tr("back lens");
}

} // namespace

LensCalibrationDialog::LensCalibrationDialog(const QString &startDir, LensGuard guard,
                                             QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("lensCalibrationDialog"));
    setWindowTitle(tr("Calibrate lenses"));
    m_pages = new QStackedWidget(this);

    // 1. The chart.
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        v->addWidget(Text(
            tr("<p>orbis-reframe measures your camera's two lenses from a video of a chart, "
               "so their pictures line up better than with the lens model the camera "
               "records. You need the camera, a monitor and about 20 minutes. Do it once for "
               "every set of lens guards you film with.</p>"
               "<p><b>1. Show the chart</b> full screen on a monitor, then measure one of its "
               "squares with a ruler.</p>"),
            page));
        auto *form = new QFormLayout;
        m_screen = new QComboBox(page);
        m_screen->setObjectName(QStringLiteral("lensCalibrationScreen"));
        for (QScreen *s : QGuiApplication::screens())
            m_screen->addItem(QStringLiteral("%1 (%2 × %3)")
                                  .arg(s->name())
                                  .arg(std::lround(s->geometry().width() * s->devicePixelRatio()))
                                  .arg(std::lround(s->geometry().height() * s->devicePixelRatio())));
        form->addRow(tr("Monitor"), m_screen);
        m_showChart = new QPushButton(tr("Show the chart"), page);
        m_showChart->setObjectName(QStringLiteral("lensCalibrationShowChart"));
        form->addRow(QString(), m_showChart);
        m_square = new QDoubleSpinBox(page);
        m_square->setObjectName(QStringLiteral("lensCalibrationSquare"));
        m_square->setRange(0, 200);
        m_square->setDecimals(2);
        m_square->setSingleStep(0.1);
        m_square->setSuffix(tr(" mm"));
        form->addRow(tr("Square size"), m_square);
        v->addLayout(form);
        m_squareHint = Text(QString(), page);
        m_squareHint->setObjectName(QStringLiteral("dialogHint"));
        v->addWidget(m_squareHint);
        v->addStretch(1);
        m_pages->addWidget(page);
        connect(m_screen, &QComboBox::currentIndexChanged, this, &LensCalibrationDialog::screenChosen);
        connect(m_showChart, &QPushButton::clicked, this, &LensCalibrationDialog::showChart);
        connect(m_square, &QDoubleSpinBox::valueChanged, this, &LensCalibrationDialog::updateButtons);
    }
    // 2. Filming.
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        v->addWidget(Text(
            tr("<p><b>2. Film the chart.</b></p>"
               "<ul>"
               "<li>Fit the lens guards you want to calibrate for (or none), and film in 8K.</li>"
               "<li>Hold the camera 50 cm to 1 m from the screen, in a room without strong "
               "reflections on it.</li>"
               "<li>Turn the camera slowly through five full circles, so the chart passes "
               "through every part of both lenses' pictures, out to their edges: turning left "
               "to right; tilting forwards over the top; left to right again with the camera "
               "leaning 45° one way, then 45° the other way; and once round with the line "
               "where the lenses meet (the camera's sides) passing over the chart.</li>"
               "<li>Stop for a moment every few seconds: only the frames where the chart "
               "holds still are used.</li>"
               "<li>About three minutes is plenty.</li>"
               "</ul>"),
            page));
        v->addStretch(1);
        m_pages->addWidget(page);
    }
    // 3. Measuring.
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        v->addWidget(Text(tr("<p><b>3. Measure the recording.</b> This takes a few minutes, "
                             "about twice as long as the recording.</p>"),
                          page));
        auto *form = new QFormLayout;
        auto *row = new QHBoxLayout;
        m_recording = new QLineEdit(page);
        m_recording->setObjectName(QStringLiteral("lensCalibrationRecording"));
        m_recording->setPlaceholderText(tr("The recording of the chart (.insv)"));
        auto *browse = new QPushButton(tr("Choose…"), page);
        row->addWidget(m_recording, 1);
        row->addWidget(browse);
        form->addRow(tr("Recording"), row);
        m_guard = new QComboBox(page);
        m_guard->setObjectName(QStringLiteral("lensCalibrationGuard"));
        m_guard->addItem(tr("None"));
        m_guard->addItem(tr("Standard lens guards"));
        m_guard->addItem(tr("Premium lens guards"));
        m_guard->setCurrentIndex(static_cast<int>(guard));
        m_guard->setToolTip(tr("What was over the lenses while you filmed the chart."));
        form->addRow(tr("Lens guards"), m_guard);
        v->addLayout(form);
        m_progress = new QProgressBar(page);
        m_progress->setRange(0, 1000);
        m_progress->setTextVisible(false);
        m_progress->hide();
        v->addWidget(m_progress);
        m_status = Text(QString(), page);
        v->addWidget(m_status);
        v->addStretch(1);
        m_pages->addWidget(page);
        connect(browse, &QPushButton::clicked, this, &LensCalibrationDialog::browse);
        connect(m_recording, &QLineEdit::textChanged, this, &LensCalibrationDialog::updateButtons);
        m_recording->setProperty("startDir", startDir);
    }
    // 4. The result.
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        m_result = Text(QString(), page);
        m_result->setObjectName(QStringLiteral("lensCalibrationResult"));
        v->addWidget(m_result);
        m_useInProject = new QCheckBox(tr("Use it for this project's clips from this camera"), page);
        m_useInProject->setChecked(true);
        v->addWidget(m_useInProject);
        v->addStretch(1);
        m_pages->addWidget(page);
    }

    m_back = new QPushButton(tr("Back"), this);
    m_next = new QPushButton(tr("Next"), this);
    m_next->setObjectName(QStringLiteral("lensCalibrationNext"));
    m_next->setDefault(true);
    m_save = new QPushButton(tr("Save"), this);
    m_save->setObjectName(QStringLiteral("lensCalibrationSave"));
    m_close = new QPushButton(tr("Close"), this);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_back);
    buttons->addStretch(1);
    buttons->addWidget(m_next);
    buttons->addWidget(m_save);
    buttons->addWidget(m_close);
    connect(m_back, &QPushButton::clicked, this, [this] {
        if (m_worker)
            return;
        showPage(std::max(0, m_pages->currentIndex() - 1));
    });
    connect(m_next, &QPushButton::clicked, this, [this] {
        if (m_pages->currentIndex() == MeasurePage)
            m_worker ? stop() : start();
        else
            showPage(m_pages->currentIndex() + 1);
    });
    connect(m_save, &QPushButton::clicked, this, &LensCalibrationDialog::save);
    connect(m_close, &QPushButton::clicked, this, &LensCalibrationDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages, 1);
    layout->addLayout(buttons);
    setMinimumSize(560, 420);
    screenChosen();
    showPage(ChartPage);
}

LensCalibrationDialog::~LensCalibrationDialog()
{
    if (m_worker) {
        m_cancel = true;
        m_worker->wait();
    }
    delete m_chart;
}

void LensCalibrationDialog::reject()
{
    if (m_worker) {
        m_cancel = true;
        m_worker->wait();
    }
    if (m_chart)
        m_chart->close();
    QDialog::reject();
}

void LensCalibrationDialog::showPage(int page)
{
    m_pages->setCurrentIndex(page);
    updateButtons();
}

void LensCalibrationDialog::updateButtons()
{
    const int page = m_pages->currentIndex();
    m_back->setVisible(page != ChartPage);
    m_back->setEnabled(!m_worker);
    m_next->setVisible(page != ResultPage);
    m_save->setVisible(page == ResultPage);
    m_save->setEnabled(m_passed);
    switch (page) {
    case ChartPage:
        m_next->setText(tr("Next"));
        m_next->setEnabled(m_square->value() > 0);
        break;
    case FilmPage:
        m_next->setText(tr("Next"));
        m_next->setEnabled(true);
        break;
    case MeasurePage:
        m_next->setText(m_worker ? tr("Stop") : tr("Measure"));
        m_next->setEnabled(m_worker || QFileInfo(m_recording->text()).isFile());
        break;
    }
}

void LensCalibrationDialog::screenChosen()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    const int i = m_screen->currentIndex();
    if (i < 0 || i >= screens.size())
        return;
    QScreen *s = screens.at(i);
    const qreal dpr = s->devicePixelRatio();
    const int w = int(std::lround(s->geometry().width() * dpr));
    const int h = int(std::lround(s->geometry().height() * dpr));
    const int sq = calib::MakeChartImage(w, h).squarePx;
    const double mmPerPx = s->physicalSize().width() > 0 ? s->physicalSize().width() / w : 0;
    m_square->setValue(sq * mmPerPx);
    m_squareHint->setText(
        mmPerPx > 0
            ? tr("The monitor says this is the size of a square. Measure one: if it's "
                 "different, put in what you measure (to a tenth of a millimetre is plenty).")
            : tr("The monitor doesn't say its size: show the chart and measure one of its "
                 "squares."));
    updateButtons();
}

void LensCalibrationDialog::showChart()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    const int i = m_screen->currentIndex();
    if (i < 0 || i >= screens.size())
        return;
    delete m_chart;
    m_chart = new ChartWindow(screens.at(i));
    m_chart->showFullScreen();
}

void LensCalibrationDialog::browse()
{
    const QString dir = m_recording->text().isEmpty()
                            ? m_recording->property("startDir").toString()
                            : QFileInfo(m_recording->text()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("The recording of the chart"), dir,
                                                      tr("360 videos (*.insv);;All files (*)"));
    if (!path.isEmpty())
        m_recording->setText(path);
}

void LensCalibrationDialog::start()
{
    const std::string path = m_recording->text().toStdString();
    std::string err;
    const auto clip = camera::ReadClipMetadata(path, &err);
    if (!clip || !clip->calibrated || clip->lenses.size() != 2) {
        m_status->setText(tr("This isn't a recording orbis-reframe can calibrate from: %1")
                              .arg(clip ? tr("its lens model can't be read") : QString::fromStdString(err)));
        return;
    }
    if (clip->serial.empty()) {
        m_status->setText(tr("The recording doesn't say which camera filmed it."));
        return;
    }
    m_cancel = false;
    m_passed = false;
    m_calibration.reset();
    m_progress->setValue(0);
    m_progress->show();
    m_status->setText(tr("Finding the chart…"));
    const double square = m_square->value();
    const int guard = m_guard->currentIndex();
    m_worker = QThread::create([this, path, clip, square, guard] {
        auto report = [this](double share, bool fitting) {
            QMetaObject::invokeMethod(this, [this, share, fitting] {
                m_progress->setValue(int(1000 * share));
                if (fitting)
                    m_status->setText(tr("Fitting the lens model…"));
            }, Qt::QueuedConnection);
        };
        calib::ChartObservations obs;
        std::string err;
        bool ok = calib::DetectChart(path, obs, err, [&](double p) { report(0.9 * p, false); },
                                     &m_cancel);
        calib::LensFitResult r;
        if (ok) {
            calib::LensFitOptions opt;
            opt.squareMm = square;
            opt.lensGuard = guard;
            r = calib::FitLensCalibration(*clip, obs, opt,
                                          [&](double p) { report(0.9 + 0.1 * p, true); }, &m_cancel);
            ok = r.ok;
            err = r.message;
        }
        QMetaObject::invokeMethod(this, [this, ok, err, r, clip, guard] {
            if (ok) {
                stitch::LensCalibration c = r.calibration;
                c.serial = clip->serial;
                c.lensGuard = guard;
                c.created = QDateTime::currentDateTimeUtc()
                                .toString(Qt::ISODate)
                                .toStdString();
                m_calibration = c;
                const std::vector<calib::LensFitProblem> problems = calib::CheckLensFit(r);
                m_passed = problems.empty();
                QString html;
                const QLocale loc;
                const auto deg = [&loc](double v, int d = 2) { return loc.toString(v, 'f', d); };
                if (m_passed)
                    html += tr("<p><b>The calibration fits.</b></p>");
                else
                    html += tr("<p><b>The recording isn't good enough to calibrate from.</b></p><ul>");
                for (const calib::LensFitProblem p : problems) {
                    switch (p) {
                    case calib::LensFitProblem::TooFewFrames:
                        html += tr("<li>The chart was still in too few frames: turn the camera "
                                   "more slowly, and stop for a moment every few seconds.</li>");
                        break;
                    case calib::LensFitProblem::CoverageLens0:
                    case calib::LensFitProblem::CoverageLens1: {
                        const int l = p == calib::LensFitProblem::CoverageLens0 ? 0 : 1;
                        html += tr("<li>The %1 never saw the chart near the edge of its picture "
                                   "(only to %2° from its centre): turn the camera further, so "
                                   "the chart passes the edge of each lens.</li>")
                                    .arg(LensName(l), deg(c.coverageDeg[l], 0));
                        break;
                    }
                    case calib::LensFitProblem::Rms:
                        html += tr("<li>The chart's corners don't fit the lens model closely "
                                   "enough: check the square size, and that the chart is sharp "
                                   "and free of reflections.</li>");
                        break;
                    case calib::LensFitProblem::Transfer:
                        html += tr("<li>The two lenses don't agree on where the chart is: was "
                                   "the camera moving fast? Stop for a moment more often.</li>");
                        break;
                    }
                }
                if (!m_passed)
                    html += QStringLiteral("</ul>");
                html += tr("<p>Both lenses saw the chart out to %1° and %2° from their centres "
                           "(%3 and %4 still frames); its corners fit the lens model to %5 and "
                           "%6 pixels on average (the camera file's: %7 and %8).</p>")
                            .arg(deg(c.coverageDeg[0], 0), deg(c.coverageDeg[1], 0))
                            .arg(r.framesUsed[0])
                            .arg(r.framesUsed[1])
                            .arg(deg(c.rmsPx[0]), deg(c.rmsPx[1]), deg(r.fileRmsPx[0]),
                                 deg(r.fileRmsPx[1]));
                html += tr("<p>Against the camera file's lens model, it moves the picture by "
                           "%1° (front lens) and %2° (back lens) where the lenses meet, at most "
                           "%3°.</p>")
                            .arg(deg(r.shiftDeg[0]), deg(r.shiftDeg[1]),
                                 deg(std::max(r.shiftMaxDeg[0], r.shiftMaxDeg[1])));
                if (m_passed)
                    html += tr("<p>Save it to use it for this camera's clips filmed with these "
                               "lens guards. A new calibration replaces the last one.</p>");
                m_result->setText(html);
                m_useInProject->setVisible(m_passed);
                core::log::Info("[lens calibration] %s: rms %.3f / %.3f px, transfer %.3f deg, "
                                "coverage %.0f / %.0f deg, %s\n",
                                c.serial.c_str(), c.rmsPx[0], c.rmsPx[1], c.transferDeg,
                                c.coverageDeg[0], c.coverageDeg[1], m_passed ? "passed" : "failed");
            }
            finished(ok, QString::fromStdString(err));
        }, Qt::QueuedConnection);
    });
    connect(m_worker, &QThread::finished, m_worker, &QObject::deleteLater);
    m_worker->start(QThread::LowPriority);
    updateButtons();
}

void LensCalibrationDialog::stop()
{
    m_cancel = true;
}

void LensCalibrationDialog::finished(bool ok, const QString &message)
{
    m_worker = nullptr;
    m_progress->hide();
    if (ok) {
        m_status->clear();
        showPage(ResultPage);
        return;
    }
    m_status->setText(m_cancel ? tr("Stopped.") : tr("Couldn't measure it: %1").arg(message));
    updateButtons();
}

void LensCalibrationDialog::save()
{
    if (!m_calibration || !m_passed)
        return;
    QString error;
    if (!lenscal::Save(*m_calibration, &error)) {
        QMessageBox::warning(this, tr("Could not save the calibration"), error);
        return;
    }
    emit calibrationSaved(*m_calibration, m_useInProject->isChecked());
    accept();
}
