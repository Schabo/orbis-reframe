#include "StitchingPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStandardItemModel>

StitchingPanel::StitchingPanel(QWidget *parent)
    : SettingsPanel(tr("Stitching"), parent)
{
    // Order matches stitch::PreviewStitchConfig::LensGuard.
    m_lensGuard = new QComboBox(this);
    m_lensGuard->setObjectName(QStringLiteral("stitchLensGuard"));
    m_lensGuard->addItem(tr("None"));
    m_lensGuard->addItem(tr("Standard lens guards"));
    m_lensGuard->addItem(tr("Premium lens guards"));
    m_lensGuard->setToolTip(tr("What was fitted over the lenses when you filmed this clip. The "
                               "file doesn't say, and with the wrong choice the two pictures "
                               "don't line up at the seam."));
    form()->addRow(tr("Lens guards"), m_lensGuard);

    m_noGuards = new QLabel(tr("This camera records no lens-guard profiles, so its lenses are "
                               "stitched bare."),
                            this);
    m_noGuards->setObjectName(QStringLiteral("stitchNoGuards"));
    m_noGuards->setWordWrap(true);
    m_noGuards->hide();
    form()->addRow(m_noGuards);

    m_depthSeam = new QCheckBox(tr("Depth-aware seam"), this);
    m_depthSeam->setObjectName(QStringLiteral("stitchDepthSeam"));
    m_depthSeam->setToolTip(
        tr("The two lenses sit a few centimetres apart, so things close to the camera (the "
           "bike, the rider, the stick) show up twice where the pictures meet. With this on, "
           "orbis-reframe works out for every frame how far away the things along the seam "
           "are, and lines the pictures up at that distance."));
    form()->addRow(QString(), m_depthSeam);

    m_calibration = new QComboBox(this);
    m_calibration->setObjectName(QStringLiteral("stitchCalibration"));
    m_calibration->setToolTip(
        tr("The lens model the two pictures are joined with: the one the camera recorded in "
           "the file, or one made for this camera with Calibrate lenses."));
    form()->addRow(tr("Lens calibration"), m_calibration);
    connect(m_calibration, &QComboBox::activated, this, [this](int index) {
        if (!m_updating)
            emit calibrationChosen(index);
    });

    m_seamStatus = new QLabel(this);
    m_seamStatus->setWordWrap(true);
    m_seamStatus->hide();
    form()->addRow(m_seamStatus);

    auto *buttonRow = new QWidget(this);
    auto *buttons = new QHBoxLayout(buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    m_applyToAll = new QPushButton(tr("Apply to all clips"), buttonRow);
    m_applyToAll->setObjectName(QStringLiteral("stitchApplyToAll"));
    m_applyToAll->setToolTip(tr("Give every 360° clip these stitching settings: for footage "
                                "all filmed with the same lens guards."));
    buttons->addWidget(m_applyToAll);
    m_calibrate = new QPushButton(tr("Calibrate lenses…"), buttonRow);
    m_calibrate->setObjectName(QStringLiteral("stitchCalibrate"));
    m_calibrate->setToolTip(tr("Measure your camera's lenses with a chart on your monitor, so "
                               "the two pictures line up better than with the lens model the "
                               "camera records."));
    buttons->addWidget(m_calibrate);
    buttons->addStretch(1);
    connect(m_calibrate, &QPushButton::clicked, this, &StitchingPanel::calibrateRequested);
    form()->addRow(buttonRow);
    connect(m_applyToAll, &QPushButton::clicked, this, &StitchingPanel::applyToAllRequested);

    setHint(tr("How this clip's two lens pictures are joined into the 360° view, in playback, "
               "screenshots and export. Every clip cut from the same file shares them. The "
               "preview's own quality and size are in the View menu."));

    setState(LensGuard::Premium, true); // sane starting state
    setCalibrations({tr("From the camera file")}, {}, 0);

    connect(m_lensGuard, &QComboBox::currentIndexChanged, this, &StitchingPanel::emitIfEdited);
    connect(m_depthSeam, &QCheckBox::toggled, this, &StitchingPanel::emitIfEdited);
}

void StitchingPanel::setState(LensGuard lensGuard, bool depthSeam,
                              stitch::LensGuardProfiles profiles)
{
    setBodyEnabled(true);
    m_updating = true;
    auto *model = qobject_cast<QStandardItemModel *>(m_lensGuard->model());
    for (const LensGuard g : {LensGuard::Standard, LensGuard::Premium})
        model->item(static_cast<int>(g))->setEnabled(profiles.has(g));
    m_lensGuard->setEnabled(profiles.any());
    m_noGuards->setVisible(!profiles.any());
    m_lensGuard->setCurrentIndex(static_cast<int>(profiles.usable(lensGuard)));
    m_depthSeam->setChecked(depthSeam);
    m_updating = false;
}

void StitchingPanel::setNoClip()
{
    setState(LensGuard::Premium, true); // ClipItem's defaults
    setBodyEnabled(false);
}

void StitchingPanel::setCalibrations(const QStringList &labels, const QStringList &tooltips,
                                     int current)
{
    m_updating = true;
    m_calibration->clear();
    for (int i = 0; i < labels.size(); ++i) {
        m_calibration->addItem(labels.at(i));
        if (i < tooltips.size() && !tooltips.at(i).isEmpty())
            m_calibration->setItemData(i, tooltips.at(i), Qt::ToolTipRole);
    }
    m_calibration->setCurrentIndex(current);
    m_calibration->setEnabled(labels.size() > 1);
    m_updating = false;
}

void StitchingPanel::setCalibrateAvailable(bool available, const QString &whyNot)
{
    m_calibrate->setEnabled(available);
    if (!available && !whyNot.isEmpty())
        m_calibrate->setToolTip(whyNot);
}

void StitchingPanel::setSeamStatus(const QString &text, const QString &details)
{
    const QString about = tr("orbis-reframe lines the two pictures up at the seam, and matches "
                             "their colours, from the video itself, once for each clip, in the "
                             "background.");
    m_seamStatus->setText(text);
    m_seamStatus->setToolTip(details.isEmpty() ? about : about + QStringLiteral("\n\n") + details);
    m_seamStatus->setVisible(!text.isEmpty());
}

void StitchingPanel::emitIfEdited()
{
    if (!m_updating)
        emit stitchEdited(static_cast<LensGuard>(m_lensGuard->currentIndex()),
                          m_depthSeam->isChecked());
}
