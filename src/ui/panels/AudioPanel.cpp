#include "AudioPanel.h"

#include "../../core/AudioLimits.h"

#include <NoiseFilter.h> // insv::voiceModesReady

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

// Debian's ONNX Runtime package for the version built against (CMakeLists.txt).
#ifdef INSV_ORT_PACKAGE
const char *const kOrtPackage = INSV_ORT_PACKAGE;
#else
const char *const kOrtPackage = "libonnxruntime";
#endif

// A dB slider with its value label, in one row widget.
QWidget *gainRow(QWidget *parent, double minDb, double maxDb, const QString &tip, QSlider **slider,
                 QLabel **value)
{
    auto *row = new QWidget(parent);
    auto *gl = new QHBoxLayout(row);
    gl->setContentsMargins(0, 0, 0, 0);
    *slider = new QSlider(Qt::Horizontal, row);
    (*slider)->setRange(int(minDb), int(maxDb));
    (*slider)->setSingleStep(1);
    (*slider)->setPageStep(3);
    (*slider)->setTickPosition(QSlider::TicksBelow);
    (*slider)->setTickInterval(6);
    (*slider)->setToolTip(tip);
    *value = new QLabel(row);
    (*value)->setMinimumWidth(row->fontMetrics().horizontalAdvance(QStringLiteral("-24 dB")));
    gl->addWidget(*slider, 1);
    gl->addWidget(*value);
    return row;
}

QDoubleSpinBox *secondsBox(QWidget *parent, int decimals, double step)
{
    auto *b = new QDoubleSpinBox(parent);
    b->setDecimals(decimals);
    b->setSingleStep(step);
    b->setSuffix(QStringLiteral(" s"));
    b->setKeyboardTracking(false); // commit on Enter / focus-out / arrows, not per keystroke
    return b;
}

} // namespace

AudioPanel::AudioPanel(QWidget *parent)
    : SettingsPanel(tr("Audio"), parent)
{
    // The track first (it holds the clip), a separator, then the clip itself.
    m_trackName = new QLabel(this);
    form()->addRow(tr("Track"), m_trackName);
    form()->addRow(tr("Track level"),
                   gainRow(this, audiolimits::kMinGainDb, audiolimits::kMaxGainDb,
                           tr("The whole track's level, on top of each clip's own."),
                           &m_trackGain, &m_trackGainValue));
    m_trackMute = new QCheckBox(tr("Mute track"), this);
    form()->addRow(QString(), m_trackMute);
    m_separator = new QFrame(this);
    m_separator->setFrameShape(QFrame::HLine);
    m_separator->setFrameShadow(QFrame::Sunken);
    form()->addRow(m_separator);

    m_clipName = new QLabel(this);
    m_clipName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // A long file name mustn't widen the sidebar: it may be cut short (the
    // tooltip has it whole).
    m_clipName->setMinimumWidth(1);
    form()->addRow(tr("Clip"), m_clipName);

    form()->addRow(tr("Level"), gainRow(this, audiolimits::kMinGainDb, audiolimits::kMaxGainDb,
                                        tr("This clip's audio level. 0 dB = unchanged."), &m_gain,
                                        &m_gainValue));
    m_mute = new QCheckBox(tr("Mute"), this);
    form()->addRow(QString(), m_mute);

    m_noiseMode = new QComboBox(this);
    m_noiseMode->addItem(tr("Off"), int(insv::NoiseMode::Off));
    m_noiseMode->addItem(tr("Wind reduction"), int(insv::NoiseMode::Wind));
    m_noiseMode->addItem(tr("MC: Wind reduction"), int(insv::NoiseMode::MotorcycleWind));
    m_noiseMode->addItem(tr("Hiss / hum"), int(insv::NoiseMode::Hiss));
#ifdef INSV_HAVE_VOICE
    // Listed even when ONNX Runtime or the model is missing: picking one then
    // says why it can't run (below) instead of the modes just not being there.
    m_noiseMode->addItem(tr("Voice only"), int(insv::NoiseMode::Voice));
    m_noiseMode->addItem(tr("Voice + ambience"), int(insv::NoiseMode::VoiceAmbience));
#endif
    m_noiseMode->setToolTip(
        tr("Noise reduction for this clip, in playback and export.\n"
           "Wind reduction: turns down wind noise, and keeps voices and other sounds that "
           "stand out above it.\n"
           "MC: Wind reduction: for motorcycles. Keeps the engine's sound and turns down the "
           "rumble around it, wind most of all. Works best on a mic that's sheltered from the "
           "wind.\n"
           "Hiss / hum: for quiet scenes. Turns down steady background noise and mains hum. "
           "Not for riding: a steady engine counts as background too.\n"
           "Voice only: keeps speech and removes everything else.\n"
           "Voice + ambience: keeps speech and turns everything else down evenly.\n"
           "The voice modes make jumping to another point take a moment longer."));
    form()->addRow(tr("Noise"), m_noiseMode);
    auto *strengthRow = new QWidget(this);
    auto *sl = new QHBoxLayout(strengthRow);
    sl->setContentsMargins(0, 0, 0, 0);
    m_noiseStrength = new QSlider(Qt::Horizontal, strengthRow);
    m_noiseStrength->setRange(0, 100);
    m_noiseStrength->setSingleStep(5);
    m_noiseStrength->setPageStep(10);
    m_noiseStrength->setTickPosition(QSlider::TicksBelow);
    m_noiseStrength->setTickInterval(10);
    m_noiseStrength->setToolTip(tr("How much of the noise to take out. Choosing a mode sets "
                                   "its usual value: 70 % for the wind modes, 50 % for \"Hiss "
                                   "/ hum\" and \"Voice + ambience\", 100 % for \"Voice only\"."));
    m_noiseStrengthValue = new QLabel(strengthRow);
    m_noiseStrengthValue->setMinimumWidth(
        strengthRow->fontMetrics().horizontalAdvance(QStringLiteral("100 %")));
    sl->addWidget(m_noiseStrength, 1);
    sl->addWidget(m_noiseStrengthValue);
    form()->addRow(tr("Strength"), strengthRow);

    m_fadeIn = secondsBox(this, 1, 0.5);
    m_fadeIn->setToolTip(tr("Fade in from silence over this long at the clip's start."));
    form()->addRow(tr("Fade in"), m_fadeIn);
    m_fadeOut = secondsBox(this, 1, 0.5);
    m_fadeOut->setToolTip(tr("Fade out to silence over this long at the clip's end."));
    form()->addRow(tr("Fade out"), m_fadeOut);
    m_start = secondsBox(this, 3, 0.01);
    m_start->setRange(0.0, 24 * 3600.0);
    m_start->setToolTip(tr("Where the clip starts on the timeline. The arrows nudge it by 10 "
                           "ms, for example to line a separate mic up with the picture."));
    form()->addRow(tr("Start"), m_start);

    setHint(tr("The audio clip selected on the timeline, or else the sound of the video clip "
               "under the playhead. Applies in playback and export."));
    setNoClip();

    connect(m_gain, &QSlider::valueChanged, this, [this](int v) {
        m_gainValue->setText(tr("%1 dB").arg(v));
        emitIfEdited();
    });
    connect(m_mute, &QCheckBox::toggled, this, [this](bool) { emitIfEdited(); });
    connect(m_noiseMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto mode = static_cast<insv::NoiseMode>(m_noiseMode->currentData().toInt());
        if (!m_updating && (mode == insv::NoiseMode::Voice || mode == insv::NoiseMode::VoiceAmbience)) {
            // Loads ONNX Runtime and the model if needed (~0.5 s, once).
            QApplication::setOverrideCursor(Qt::WaitCursor);
            std::string why;
            const bool ready = insv::voiceModesReady(&why);
            QApplication::restoreOverrideCursor();
            if (!ready) {
                {
                    const QSignalBlocker block(m_noiseMode);
                    m_noiseMode->setCurrentIndex(m_lastNoiseIndex);
                }
                QMessageBox::warning(
                    this, tr("Noise reduction"),
                    tr("The voice modes can't run on this computer: %1\n\nThey need ONNX Runtime "
                       "(on Debian: sudo apt install %2) and the DeepFilterNet "
                       "model that comes with orbis-reframe (reinstall orbis-reframe if it's "
                       "missing).")
                        .arg(QString::fromStdString(why), QString::fromLatin1(kOrtPackage)));
                return;
            }
        }
        m_lastNoiseIndex = index;
        m_noiseStrength->parentWidget()->setEnabled(mode != insv::NoiseMode::Off);
        if (!m_updating && mode != insv::NoiseMode::Off) {
            // A newly picked mode starts at its own default strength.
            const QSignalBlocker block(m_noiseStrength);
            const int pct = int(std::lround(insv::NoiseReduction::defaultStrength(mode) * 100.0f));
            m_noiseStrength->setValue(pct);
            m_noiseStrengthValue->setText(tr("%1 %").arg(pct));
        }
        emitNoiseIfEdited();
    });
    connect(m_noiseStrength, &QSlider::valueChanged, this, [this](int v) {
        m_noiseStrengthValue->setText(tr("%1 %").arg(v));
        emitNoiseIfEdited();
    });
    connect(m_fadeIn, &QDoubleSpinBox::valueChanged, this, [this](double) { emitIfEdited(); });
    connect(m_fadeOut, &QDoubleSpinBox::valueChanged, this, [this](double) { emitIfEdited(); });
    connect(m_start, &QDoubleSpinBox::valueChanged, this, [this](double) { emitIfEdited(); });
    connect(m_trackGain, &QSlider::valueChanged, this, [this](int v) {
        m_trackGainValue->setText(tr("%1 dB").arg(v));
        emitTrackIfEdited();
    });
    connect(m_trackMute, &QCheckBox::toggled, this, [this](bool) { emitTrackIfEdited(); });
}

void AudioPanel::setAudioClipState(const AudioClipState &s)
{
    m_updating = true;
    setBodyEnabled(true);
    setScope(s.selected ? tr("Selected audio clip") : tr("This clip"));
    m_clipName->setText(s.label);
    m_clipName->setToolTip(s.label);
    const int v = std::clamp(int(std::lround(s.gainDb)), int(audiolimits::kMinGainDb),
                             int(audiolimits::kMaxGainDb));
    m_gain->setValue(v);
    m_gainValue->setText(tr("%1 dB").arg(v));
    m_mute->setChecked(s.muted);
    m_spanMs = s.spanMs;
    m_fadeIn->setRange(0.0, s.spanMs / 1000.0);
    m_fadeOut->setRange(0.0, s.spanMs / 1000.0);
    m_fadeIn->setValue(s.fadeInMs / 1000.0);
    m_fadeOut->setValue(s.fadeOutMs / 1000.0);
    m_start->setValue(s.startMs / 1000.0);
    m_start->setEnabled(!s.linked);
    m_start->setToolTip(
        s.linked ? tr("Linked to its video clip, so it moves with it and can't be moved on its "
                      "own. To re-sync it: right-click it, Unlink, move it, then Link again.")
                 : tr("Where the clip starts on the timeline. The arrows nudge it by 10 ms, "
                      "for example to line a separate mic up with the picture."));
    m_trackName->setText(s.trackName);
    const int tv = std::clamp(int(std::lround(s.trackGainDb)), int(audiolimits::kMinGainDb),
                              int(audiolimits::kMaxGainDb));
    m_trackGain->setValue(tv);
    m_trackGainValue->setText(tr("%1 dB").arg(tv));
    m_trackMute->setChecked(s.trackMuted);
    setNoise(s.noise);
    m_updating = false;
}

// Called with m_updating set: reflects `n`, doesn't emit.
void AudioPanel::setNoise(const insv::NoiseReduction &n)
{
    m_noiseMode->setCurrentIndex(std::max(0, m_noiseMode->findData(int(n.mode))));
    m_lastNoiseIndex = m_noiseMode->currentIndex();
    const int pct = int(std::lround(std::clamp(n.strength, 0.0f, 1.0f) * 100.0f));
    m_noiseStrength->setValue(pct);
    m_noiseStrengthValue->setText(tr("%1 %").arg(pct));
    m_noiseStrength->parentWidget()->setEnabled(n.mode != insv::NoiseMode::Off);
}

void AudioPanel::emitNoiseIfEdited()
{
    if (m_updating)
        return;
    insv::NoiseReduction n;
    n.mode = static_cast<insv::NoiseMode>(m_noiseMode->currentData().toInt());
    n.strength = m_noiseStrength->value() / 100.0f;
    emit noiseEdited(n);
}

void AudioPanel::setNoClip()
{
    setBodyEnabled(false);
}

void AudioPanel::emitIfEdited()
{
    if (m_updating)
        return;
    emit audioClipEdited(m_gain->value(), m_mute->isChecked(),
                         std::llround(m_fadeIn->value() * 1000.0),
                         std::llround(m_fadeOut->value() * 1000.0),
                         std::llround(m_start->value() * 1000.0));
}

void AudioPanel::emitTrackIfEdited()
{
    if (!m_updating)
        emit trackLevelEdited(m_trackGain->value(), m_trackMute->isChecked());
}
