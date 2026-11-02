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

    // Order matches stitch::PreviewStitchConfig::Method.
    m_method = new QComboBox(this);
    m_method->setObjectName(QStringLiteral("stitchMethod"));
    m_method->addItem(tr("Depth-aware seam"));
    m_method->setItemData(
        0,
        tr("The two lenses sit a few centimetres apart, so things close to the camera (the "
           "bike, the rider, the stick) show up twice where the pictures meet. This works out "
           "for every frame how far away the things along the seam are, and lines the "
           "pictures up at that distance."),
        Qt::ToolTipRole);
    m_method->addItem(tr("Fixed seam"));
    m_method->setItemData(1,
                          tr("The pictures lined up for things far away, the same in every "
                             "frame: steady, but things close to the camera show up twice "
                             "where the pictures meet."),
                          Qt::ToolTipRole);
    m_method->addItem(tr("New stitcher (experimental)"));
    m_method->setItemData(
        2,
        tr("The stitcher that will replace the other two, still being built. It joins the "
           "pictures from the lens calibration, and works out anything more from each frame "
           "and the ones just before it, not from the clip as a whole."),
        Qt::ToolTipRole);
    m_method->setToolTip(tr("How the two pictures are joined where they meet."));
    form()->addRow(tr("Stitcher"), m_method);

    m_seamColourBlend = new QCheckBox(tr("Seam colour blend"), this);
    m_seamColourBlend->setObjectName(QStringLiteral("stitchSeamColourBlend"));
    m_seamColourBlend->setToolTip(
        tr("The two lenses never see quite the same colour. With this on, the new stitcher "
           "measures how they differ along the seam over the last second and evens it out "
           "there, fading to nothing about 30° away, so the rest of each picture "
           "keeps its colour."));
    form()->addRow(QString(), m_seamColourBlend);
    connect(m_seamColourBlend, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit seamColourBlendEdited(on);
    });

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

    setState(LensGuard::Premium, Method::DepthSeam); // sane starting state
    setCalibrations({tr("From the camera file")}, {}, 0);

    connect(m_lensGuard, &QComboBox::currentIndexChanged, this, &StitchingPanel::emitIfEdited);
    connect(m_method, &QComboBox::currentIndexChanged, this, &StitchingPanel::emitIfEdited);
    connect(m_method, &QComboBox::currentIndexChanged, this,
            &StitchingPanel::updateSeamColourBlendEnabled);
}

void StitchingPanel::setSeamColourBlend(bool on)
{
    m_updating = true;
    m_seamColourBlend->setChecked(on);
    m_updating = false;
}

void StitchingPanel::updateSeamColourBlendEnabled()
{
    // Only the new stitcher has it; the old one matches the lenses its own way.
    m_seamColourBlend->setEnabled(m_method->isEnabled() &&
                                  m_method->currentIndex() == static_cast<int>(Method::New));
}

void StitchingPanel::setState(LensGuard lensGuard, Method method,
                              stitch::LensGuardProfiles profiles)
{
    setBodyEnabled(true);
    m_updating = true;
    auto *model = qobject_cast<QStandardItemModel *>(m_lensGuard->model());
    for (const LensGuard g : {LensGuard::Standard, LensGuard::Premium})
        model->item(static_cast<int>(g))->setEnabled(profiles.has(g));
    m_lensGuard->setEnabled(profiles.any());
    m_noGuards->setVisible(!profiles.any());
    m_method->setEnabled(true);
    m_applyToAll->setEnabled(true);
    m_lensGuard->setCurrentIndex(static_cast<int>(profiles.usable(lensGuard)));
    m_method->setCurrentIndex(static_cast<int>(method));
    updateSeamColourBlendEnabled();
    m_updating = false;
}

void StitchingPanel::setNoClip()
{
    setState(LensGuard::Premium, Method::DepthSeam); // ClipItem's defaults
    // The clip's settings greyed out; Calibrate lenses is about the camera,
    // not a clip, so it stays.
    m_lensGuard->setEnabled(false);
    m_method->setEnabled(false);
    updateSeamColourBlendEnabled();
    m_calibration->setEnabled(false);
    m_applyToAll->setEnabled(false);
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
                          static_cast<Method>(m_method->currentIndex()));
}
