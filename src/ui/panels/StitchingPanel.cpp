#include "StitchingPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>

StitchingPanel::StitchingPanel(QWidget *parent)
    : SettingsPanel(tr("Stitching"), parent)
{
    // Order matches stitch::PreviewStitchConfig::LensGuard.
    m_lensGuard = new QComboBox(this);
    m_lensGuard->addItem(tr("None"));
    m_lensGuard->addItem(tr("Standard lens guards"));
    m_lensGuard->addItem(tr("Premium lens guards"));
    m_lensGuard->setToolTip(tr("What was fitted over the lenses when you filmed. The file "
                               "doesn't say, and with the wrong choice the two pictures don't "
                               "line up at the seam."));
    form()->addRow(tr("Lens guards"), m_lensGuard);

    m_depthSeam = new QCheckBox(tr("Depth-aware seam"), this);
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

    setHint(tr("How the two lens pictures are joined into the 360° view, in playback, "
               "screenshots and export. The preview's own quality and size are in the View "
               "menu."));

    setConfig(stitch::PreviewStitchConfig{}); // sane starting state

    connect(m_lensGuard, &QComboBox::currentIndexChanged, this, &StitchingPanel::emitIfEdited);
    connect(m_depthSeam, &QCheckBox::toggled, this, &StitchingPanel::emitIfEdited);
}

stitch::PreviewStitchConfig StitchingPanel::currentConfig() const
{
    stitch::PreviewStitchConfig c = m_config; // the preview's quality and size stay
    c.lensGuard = static_cast<stitch::PreviewStitchConfig::LensGuard>(m_lensGuard->currentIndex());
    c.depthSeam = m_depthSeam->isChecked();
    return c;
}

void StitchingPanel::setConfig(const stitch::PreviewStitchConfig &config)
{
    m_updating = true;
    m_config = config;
    m_lensGuard->setCurrentIndex(static_cast<int>(config.lensGuard));
    m_depthSeam->setChecked(config.depthSeam);
    m_updating = false;
}

void StitchingPanel::setSeamStatus(const QString &text)
{
    m_seamStatus->setText(text);
    m_seamStatus->setVisible(!text.isEmpty());
}

void StitchingPanel::emitIfEdited()
{
    if (!m_updating)
        emit configChanged(currentConfig());
}
