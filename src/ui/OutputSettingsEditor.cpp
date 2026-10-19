#include "OutputSettingsEditor.h"

#include <QComboBox>
#include <QFormLayout>
#include <QSize>

#include <algorithm>
#include <cmath>

namespace {
using Aspect = OutputSettings::Aspect;

// Combo order == OutputSettings::Aspect order.
constexpr Aspect kAspects[] = {Aspect::Landscape16x9, Aspect::Portrait9x16, Aspect::Square1x1,
                               Aspect::Classic4x3};

QString fpsLabel(double fps)
{
    // 29.97 / 59.94 / 23.976 keep their decimals; whole rates don't get ".00".
    return std::abs(fps - std::round(fps)) < 1e-6 ? QString::number(int(std::round(fps)))
                                                  : QString::number(fps, 'g', 5);
}
} // namespace

OutputSettingsEditor::OutputSettingsEditor(QWidget *parent, QFormLayout *form) : QObject(parent)
{
    m_aspect = new QComboBox(parent);
    m_aspect->setObjectName(QStringLiteral("outputAspect"));
    m_aspect->addItem(tr("16:9 Landscape"));
    m_aspect->addItem(tr("9:16 Vertical"));
    m_aspect->addItem(tr("1:1 Square"));
    m_aspect->addItem(tr("4:3"));
    m_aspect->setToolTip(tr("The shape of the video. Each shape shows a different part of the "
                            "sphere; reframe keyframes stay as they are, only what fits in the "
                            "frame changes."));
    form->addRow(tr("Aspect ratio"), m_aspect);

    m_resolution = new QComboBox(parent);
    m_resolution->setObjectName(QStringLiteral("outputResolution"));
    m_resolution->setToolTip(tr("The export's size. The export dialog can still pick another "
                                "size of the same shape."));
    form->addRow(tr("Resolution"), m_resolution);

    m_fps = new QComboBox(parent);
    m_fps->setObjectName(QStringLiteral("outputFps"));
    m_fps->addItem(QString(), 0.0); // "Match first clip", labelled by setFirstClipFps()
    for (double f : OutputSettings::fpsPresets())
        m_fps->addItem(tr("%1 fps").arg(fpsLabel(f)), f);
    m_fps->setToolTip(tr("The export's frames per second. A clip filmed at another rate has "
                         "frames left out or shown twice."));
    form->addRow(tr("Frame rate"), m_fps);

    m_bitDepth = new QComboBox(parent);
    m_bitDepth->setObjectName(QStringLiteral("outputBitDepth"));
    m_bitDepth->addItem(tr("8-bit"), false);
    m_bitDepth->addItem(tr("10-bit (H.265 only)"), true);
    m_bitDepth->setToolTip(tr("10-bit keeps the finer gradation of 10-bit clips (log footage you "
                              "grade in another program, smooth skies). 8-bit plays everywhere; "
                              "10-bit needs a player that handles HEVC Main 10."));
    form->addRow(tr("Bit depth"), m_bitDepth);

    setFirstClipFps(0.0);
    setOutput(OutputSettings{}); // sane starting state

    connect(m_aspect, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating)
            return;
        // Keep the same size class (720p / 1080p / ...) across aspects.
        const int cls = std::max(0, m_resolution->currentIndex());
        const auto sizes = OutputSettings::presets(kAspects[index]);
        const auto s = sizes[std::min<size_t>(cls, sizes.size() - 1)];
        m_updating = true;
        fillResolutions(kAspects[index], s.w, s.h);
        m_updating = false;
        emitIfEdited();
    });
    connect(m_resolution, &QComboBox::currentIndexChanged, this, &OutputSettingsEditor::emitIfEdited);
    connect(m_fps, &QComboBox::currentIndexChanged, this, &OutputSettingsEditor::emitIfEdited);
    connect(m_bitDepth, &QComboBox::currentIndexChanged, this, &OutputSettingsEditor::emitIfEdited);
}

void OutputSettingsEditor::fillResolutions(Aspect aspect, int width, int height)
{
    m_resolution->clear();
    int index = -1;
    for (const auto &s : OutputSettings::presets(aspect)) {
        if (s.w == width && s.h == height)
            index = m_resolution->count();
        m_resolution->addItem(QStringLiteral("%1 x %2").arg(s.w).arg(s.h), QSize(s.w, s.h));
    }
    if (index < 0) {
        // Not a preset (hand-edited .orp): keep it, as its own entry.
        index = m_resolution->count();
        m_resolution->addItem(tr("%1 x %2 (custom)").arg(width).arg(height),
                              QSize(width, height));
    }
    m_resolution->setCurrentIndex(index);
}

OutputSettings OutputSettingsEditor::output() const
{
    OutputSettings o;
    o.aspect = kAspects[std::max(0, m_aspect->currentIndex())];
    const QSize s = m_resolution->currentData().toSize();
    o.width = s.width();
    o.height = s.height();
    o.fps = m_fps->currentData().toDouble();
    o.tenBit = m_bitDepth->currentData().toBool();
    return o;
}

void OutputSettingsEditor::setOutput(const OutputSettings &output)
{
    m_updating = true;
    m_aspect->setCurrentIndex(static_cast<int>(output.aspect));
    fillResolutions(output.aspect, output.width, output.height);

    int fpsIndex = 0;
    for (int i = 1; i < m_fps->count(); ++i)
        if (std::abs(m_fps->itemData(i).toDouble() - output.fps) < 1e-3)
            fpsIndex = i;
    if (output.fps > 0.0 && fpsIndex == 0) {
        // Non-preset rate (hand-edited .orp): keep it, as its own entry.
        m_fps->addItem(tr("%1 fps").arg(fpsLabel(output.fps)), output.fps);
        fpsIndex = m_fps->count() - 1;
    }
    m_fps->setCurrentIndex(fpsIndex);
    m_bitDepth->setCurrentIndex(output.tenBit ? 1 : 0);
    m_updating = false;
}

void OutputSettingsEditor::setFirstClipFps(double fps)
{
    m_fps->setItemText(0, fps > 0.0 ? tr("Match first clip (%1)").arg(fpsLabel(fps))
                                    : tr("Match first clip"));
}

void OutputSettingsEditor::emitIfEdited()
{
    if (!m_updating)
        emit outputChanged(output());
}
