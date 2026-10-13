#pragma once

#include "../SettingsPanel.h"

#include <NoiseReduction.h> // insv::NoiseReduction

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QSlider;

// The Audio panel: one audio clip -- the one selected on the timeline, or
// else the sound of the video clip under the playhead (its first linked clip,
// on the stream tracks: a video clip has no sound of its own). Its level,
// mute, noise reduction (mode + strength), fades and start position, plus its
// track's level and mute (audioClipEdited / noiseEdited / trackLevelEdited
// -> Timeline::setAudioClip* / setTrackAudio).
//
// Nothing here talks to AudioPlayer directly; it all applies to playback and
// export alike.
class AudioPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit AudioPanel(QWidget *parent = nullptr);

    // No audio clip selected and none under the playhead -- greys the panel
    // out.
    void setNoClip();

    struct AudioClipState {
        QString label;
        double gainDb = 0.0;
        bool muted = false;
        qint64 fadeInMs = 0;
        qint64 fadeOutMs = 0;
        qint64 spanMs = 0; // the clip's timeline span: the fades' combined limit
        qint64 startMs = 0;
        bool linked = false; // locked to its video clip: Start is read-only
        QString trackName;
        double trackGainDb = 0.0;
        bool trackMuted = false;
        insv::NoiseReduction noise;
        bool selected = true; // picked on the timeline, not just under the playhead
    };
    // Does not emit.
    void setAudioClipState(const AudioClipState &s);

signals:
    void audioClipEdited(double gainDb, bool muted, qint64 fadeInMs, qint64 fadeOutMs,
                         qint64 startMs);
    void trackLevelEdited(double gainDb, bool muted);
    // The clip's noise reduction.
    void noiseEdited(const insv::NoiseReduction &noise);

private:
    void emitIfEdited();
    void emitTrackIfEdited();
    void setNoise(const insv::NoiseReduction &n);
    void emitNoiseIfEdited();

    QLabel *m_clipName = nullptr;
    QSlider *m_gain = nullptr;
    QLabel *m_gainValue = nullptr;
    QCheckBox *m_mute = nullptr;
    QComboBox *m_noiseMode = nullptr;
    int m_lastNoiseIndex = 0; // to go back to when a voice mode can't run
    QSlider *m_noiseStrength = nullptr; // percent
    QLabel *m_noiseStrengthValue = nullptr;
    QDoubleSpinBox *m_fadeIn = nullptr;
    QDoubleSpinBox *m_fadeOut = nullptr;
    QDoubleSpinBox *m_start = nullptr;
    QLabel *m_trackName = nullptr;
    QSlider *m_trackGain = nullptr;
    QLabel *m_trackGainValue = nullptr;
    QCheckBox *m_trackMute = nullptr;
    QFrame *m_separator = nullptr; // between the track's rows and the clip's
    qint64 m_spanMs = 0;
    bool m_updating = false;
};
