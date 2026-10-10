#include "StitchingPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QStandardItemModel>

#include <array>
#include <cstdlib>

namespace {
// 2:1 presets only -- equirect is 360x180 and the reprojection shader assumes
// it. Width is the source of truth; height is always width / 2.
// Above 3840, check real time for the clip's mode (tools/playback_bench).
constexpr std::array<int, 7> kPresetWidths{1920, 2560, 3072, 3840, 5120, 5760, 7680};
} // namespace

StitchingPanel::StitchingPanel(QWidget *parent)
    : SettingsPanel(tr("Stitching"), QStringLiteral(":/icons/layers.svg"), parent)
{
    m_quality = new QComboBox(this);
    m_quality->addItem(tr("Standard"));
    m_quality->addItem(tr("Optical flow (not yet available)"));
    // No native optical-flow seam yet: shown, not pickable.
    if (auto *model = qobject_cast<QStandardItemModel *>(m_quality->model()))
        model->item(1)->setEnabled(false);
    m_quality->setToolTip(tr("Only Standard for now. Optical flow, a smoother seam for close "
                             "objects, isn't available yet."));
    form()->addRow(tr("Quality"), m_quality);

    m_resolution = new QComboBox(this);
    m_resolution->addItem(tr("Auto (match source)"), 0);
    for (int w : kPresetWidths)
        m_resolution->addItem(QStringLiteral("%1 x %2").arg(w).arg(w / 2), w);
    m_resolution->setToolTip(
        tr("Almost never matters: the viewport normally draws straight from the two lens "
           "pictures at full detail. Only when the graphics card or driver can't do that is a "
           "flat 360° preview stitched first, and this sets its size. Auto uses the clip's "
           "full detail, as much as plays smoothly at its frame rate."));
    form()->addRow(tr("Preview size"), m_resolution);

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

    setHint(tr("How the two lens pictures are joined into the 360° view. For the whole "
               "project, in playback, screenshots and export."));

    setConfig(stitch::PreviewStitchConfig{}); // sane starting state

    connect(m_quality, &QComboBox::currentIndexChanged, this, &StitchingPanel::emitIfEdited);
    connect(m_resolution, &QComboBox::currentIndexChanged, this, &StitchingPanel::emitIfEdited);
    connect(m_lensGuard, &QComboBox::currentIndexChanged, this, &StitchingPanel::emitIfEdited);
    connect(m_depthSeam, &QCheckBox::toggled, this, &StitchingPanel::emitIfEdited);
}

stitch::PreviewStitchConfig StitchingPanel::currentConfig() const
{
    stitch::PreviewStitchConfig c;
    c.quality = m_quality->currentIndex() == 1 ? stitch::PreviewStitchConfig::Quality::OpticalFlow
                                               : stitch::PreviewStitchConfig::Quality::Template;
    const int w = m_resolution->currentData().toInt(); // 0 = Auto
    c.width = w;
    c.height = w / 2;
    c.lensGuard = static_cast<stitch::PreviewStitchConfig::LensGuard>(m_lensGuard->currentIndex());
    c.depthSeam = m_depthSeam->isChecked();
    return c;
}

void StitchingPanel::setConfig(const stitch::PreviewStitchConfig &config)
{
    m_updating = true;

    m_quality->setCurrentIndex(
        config.quality == stitch::PreviewStitchConfig::Quality::OpticalFlow ? 1 : 0);

    int resIndex = m_resolution->findData(config.width > 0 ? config.width : 0);
    if (resIndex < 0) {
        // Non-preset width (hand-edited .orp): snap to the nearest preset.
        int best = 0;
        for (int i = 1; i < static_cast<int>(kPresetWidths.size()); ++i)
            if (std::abs(kPresetWidths[i] - config.width) <
                std::abs(kPresetWidths[best] - config.width))
                best = i;
        resIndex = best + 1; // after the Auto entry
    }
    m_resolution->setCurrentIndex(resIndex);

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
