#include "ExportOptionsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QHBoxLayout>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QSize>
#include <QSpinBox>

namespace {

// Same store as the startup checks (SystemCheck.cpp).
QSettings settings()
{
    return QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"));
}

const QString kQualityKey = QStringLiteral("export/quality");
const QString kCustomMbpsKey = QStringLiteral("export/customMbps");

// Audio is AAC at a fixed rate (FlattenConfig::audio_bitrate_kbps).
constexpr qint64 kAudioKbps = 192;

} // namespace

ExportOptionsDialog::ExportOptionsDialog(int clipCount, qint64 rangeMs, int keyCount, double fps,
                                         const QStringList &keylessClips,
                                         const OutputSettings &output, QWidget *parent)
    : QDialog(parent), m_fps(fps), m_rangeMs(rangeMs)
{
    setWindowTitle(tr("Export Flat Video"));
    setModal(true);

    auto *form = new QFormLayout(this);

    const qint64 secs = rangeMs / 1000;
    const QString clips =
        clipCount > 1 ? tr("%n clips · ", nullptr, clipCount) : QString();
    auto *summary = new QLabel(
        tr("%1%2:%3 of footage · %4 · %5 fps")
            .arg(clips)
            .arg(secs / 60, 2, 10, QLatin1Char('0'))
            .arg(secs % 60, 2, 10, QLatin1Char('0'))
            .arg(keyCount == 0   ? tr("no keyframes — static forward view")
                 : keyCount == 1 ? tr("1 reframe keyframe")
                                 : tr("%1 reframe keyframes").arg(keyCount))
            .arg(QString::number(fps, 'g', 5)),
        this);
    form->addRow(tr("Exporting:"), summary);

    // Some clips keyed, some not: name the ones that will look straight
    // forward (with none keyed, the summary already says so).
    if (keyCount > 0 && !keylessClips.isEmpty()) {
        constexpr int kMaxListed = 6;
        QStringList listed = keylessClips.mid(0, kMaxListed);
        if (keylessClips.size() > kMaxListed)
            listed << tr("and %n more", nullptr, int(keylessClips.size() - kMaxListed));
        auto *warn = new QLabel(
            tr("⚠ These clips have no reframe keyframes and export a static forward view:") +
                QStringLiteral("<br>") + listed.join(QStringLiteral("<br>")),
            this);
        warn->setTextFormat(Qt::RichText);
        warn->setWordWrap(true);
        form->addRow(tr("Not reframed:"), warn);
    }

    // The project aspect's presets, plus the project size itself if it isn't
    // one (a hand-edited .orp); the project size is preselected.
    m_resolution = new QComboBox(this);
    bool projectSizeListed = false;
    for (const auto &sz : OutputSettings::presets(output.aspect))
        projectSizeListed |= (sz.w == output.width && sz.h == output.height);
    auto addSize = [&](int w, int h) {
        const bool isProject = (w == output.width && h == output.height);
        m_resolution->addItem(isProject ? tr("%1 x %2 (project)").arg(w).arg(h)
                                        : QStringLiteral("%1 x %2").arg(w).arg(h),
                              QSize(w, h));
        if (isProject)
            m_resolution->setCurrentIndex(m_resolution->count() - 1);
    };
    for (const auto &sz : OutputSettings::presets(output.aspect))
        addSize(sz.w, sz.h);
    if (!projectSizeListed)
        addSize(output.width, output.height);
    form->addRow(tr("Resolution:"), m_resolution);

    m_codec = new QComboBox(this);
    m_codec->addItem(tr("H.265 (HEVC)"));
    m_codec->addItem(tr("H.264"));
    form->addRow(tr("Codec:"), m_codec);

    // The project's bit depth preselected, as its size is; 10-bit is HEVC
    // Main 10, so H.264 is always 8-bit.
    m_bitDepth = new QComboBox(this);
    m_bitDepth->addItem(output.tenBit ? tr("8-bit") : tr("8-bit (project)"), false);
    m_bitDepth->addItem(output.tenBit ? tr("10-bit (project)") : tr("10-bit"), true);
    m_bitDepth->setCurrentIndex(output.tenBit ? 1 : 0);
    form->addRow(tr("Bit depth:"), m_bitDepth);
    const auto followCodec = [this, projectTenBit = output.tenBit] {
        const bool hevc = h265();
        m_bitDepth->setEnabled(hevc);
        m_bitDepth->setCurrentIndex(hevc && projectTenBit ? 1 : 0);
        m_bitDepth->setToolTip(hevc ? QString() : tr("10-bit needs H.265."));
    };
    connect(m_codec, &QComboBox::currentIndexChanged, this, followCodec);

    // Quality: a level, or Custom with the Mbit/s typed in. The spin box
    // shows the level's rate and is editable for Custom only; switching to
    // Custom starts from the rate shown.
    m_quality = new QComboBox(this);
    m_quality->addItem(tr("Smaller file"), int(OutputSettings::Quality::Smaller));
    m_quality->addItem(tr("Standard"), int(OutputSettings::Quality::Standard));
    m_quality->addItem(tr("High"), int(OutputSettings::Quality::High));
    m_quality->addItem(tr("Custom"), int(OutputSettings::Quality::Custom));
    m_customMbps = new QSpinBox(this);
    m_customMbps->setRange(int(OutputSettings::kMinBitrateKbps / 1000),
                           int(OutputSettings::kMaxBitrateKbps / 1000));
    m_customMbps->setSuffix(tr(" Mbit/s"));
    auto *qualityRow = new QHBoxLayout;
    qualityRow->addWidget(m_quality, 1);
    qualityRow->addWidget(m_customMbps);
    form->addRow(tr("Quality:"), qualityRow);
    m_estimate = new QLabel(this);
    form->addRow(tr("File size:"), m_estimate);

    {
        const QSettings s = settings();
        const int q = m_quality->findData(
            s.value(kQualityKey, int(OutputSettings::Quality::Standard)).toInt());
        m_quality->setCurrentIndex(q >= 0 ? q : 1);
        if (quality() == OutputSettings::Quality::Custom)
            m_customMbps->setValue(s.value(kCustomMbpsKey, 40).toInt());
    }
    updateRate();
    connect(m_resolution, &QComboBox::currentIndexChanged, this, &ExportOptionsDialog::updateRate);
    connect(m_codec, &QComboBox::currentIndexChanged, this, &ExportOptionsDialog::updateRate);
    connect(m_quality, &QComboBox::currentIndexChanged, this, &ExportOptionsDialog::updateRate);
    connect(m_customMbps, &QSpinBox::valueChanged, this, &ExportOptionsDialog::updateRate);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export…"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

int ExportOptionsDialog::outWidth() const
{
    return m_resolution->currentData().toSize().width();
}

int ExportOptionsDialog::outHeight() const
{
    return m_resolution->currentData().toSize().height();
}

OutputSettings::Quality ExportOptionsDialog::quality() const
{
    return OutputSettings::Quality(m_quality->currentData().toInt());
}

qint64 ExportOptionsDialog::bitrateKbps() const
{
    if (quality() == OutputSettings::Quality::Custom)
        return qint64(m_customMbps->value()) * 1000;
    return OutputSettings::bitrateKbpsFor(outWidth(), outHeight(), m_fps, h265(), quality());
}

void ExportOptionsDialog::updateRate()
{
    const bool custom = quality() == OutputSettings::Quality::Custom;
    m_customMbps->setEnabled(custom);
    if (!custom) {
        const QSignalBlocker block(m_customMbps);
        m_customMbps->setValue(int(bitrateKbps() / 1000));
    }
    // Duration x (video + audio rate); the container adds little.
    const double bytes = double(bitrateKbps() + kAudioKbps) * 1000.0 / 8.0 * m_rangeMs / 1000.0;
    m_estimate->setText(tr("about %1").arg(locale().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeSIFormat)));
}

void ExportOptionsDialog::rememberQuality() const
{
    QSettings s = settings();
    s.setValue(kQualityKey, int(quality()));
    if (quality() == OutputSettings::Quality::Custom)
        s.setValue(kCustomMbpsKey, m_customMbps->value());
}

bool ExportOptionsDialog::h265() const
{
    return m_codec->currentIndex() == 0;
}

bool ExportOptionsDialog::tenBit() const
{
    return h265() && m_bitDepth->currentData().toBool();
}
