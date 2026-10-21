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

    m_seamStatus = new QLabel(this);
    m_seamStatus->setWordWrap(true);
    m_seamStatus->setToolTip(tr("orbis-reframe lines the two pictures up at the seam from the "
                                "video itself, once for each clip, in the background."));
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
    buttons->addStretch(1);
    form()->addRow(buttonRow);
    connect(m_applyToAll, &QPushButton::clicked, this, &StitchingPanel::applyToAllRequested);

    setHint(tr("How this clip's two lens pictures are joined into the 360° view, in playback, "
               "screenshots and export. Every clip cut from the same file shares them. The "
               "preview's own quality and size are in the View menu."));

    setState(LensGuard::Premium, true); // sane starting state

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

void StitchingPanel::setSeamStatus(const QString &text)
{
    m_seamStatus->setText(text);
    m_seamStatus->setVisible(!text.isEmpty());
}

void StitchingPanel::emitIfEdited()
{
    if (!m_updating)
        emit stitchEdited(static_cast<LensGuard>(m_lensGuard->currentIndex()),
                          m_depthSeam->isChecked());
}
