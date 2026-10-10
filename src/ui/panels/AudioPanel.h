#pragma once

#include "../SettingsPanel.h"

#include <NoiseReduction.h> // insv::NoiseReduction

#include <QHash>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QSlider;

// The Audio panel has two modes.
//
// Clip mode: source track + level + mute of the main-track clip under
// the playhead. An edit hands the new value back to MainWindow to write onto
// that clip via Timeline::setClipAudio / setClipAudioTrack.
//
// Audio-clip mode: an audio clip selected on an extra track -- its level,
// mute, fades and start position, plus its track's level and mute
// (audioClipEdited / trackLevelEdited -> Timeline::setAudioClip* /
// setTrackAudio).
//
// Both modes also carry the clip's noise reduction (mode + strength,
// noiseEdited -> Timeline::setClipNoise / setAudioClipNoise): per mic clip.
//
// Nothing here talks to AudioPlayer directly; both modes apply to playback and
// export alike.
class AudioPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit AudioPanel(QWidget *parent = nullptr);

    // Clip mode: reflect a clip's level/mute and stream pick. `trackCount` is
    // how many audio streams the clip's source has; the picker is only enabled
    // when there's a choice. `detached` maps a stream that's also on an audio
    // track to that track's name: it can't be picked (it would play twice), and
    // if it already is, the picker says so. Does not emit.
    void setState(double gainDb, bool muted, int track, int trackCount,
                  const QString &clipLabel = QString(),
                  const QHash<int, QString> &detached = {},
                  const insv::NoiseReduction &noise = {});
    // No clip under the playhead (e.g. an empty timeline) -- greys the panel out.
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
    };
    // Audio-clip mode. Does not emit.
    void setAudioClipState(const AudioClipState &s);

signals:
    void levelEdited(double gainDb, bool muted);
    void trackEdited(int track);
    void audioClipEdited(double gainDb, bool muted, qint64 fadeInMs, qint64 fadeOutMs,
                         qint64 startMs);
    void trackLevelEdited(double gainDb, bool muted);
    // The clip's noise reduction -- the video clip's own sound in clip mode,
    // the selected audio clip's in audio-clip mode.
    void noiseEdited(const insv::NoiseReduction &noise);

private:
    void setMode(bool audioClip);
    void emitIfEdited();
    void emitTrackIfEdited();
    void setNoise(const insv::NoiseReduction &n);
    void emitNoiseIfEdited();

    bool m_audioClipMode = false;
    QLabel *m_clipName = nullptr;
    QComboBox *m_track = nullptr;
    QLabel *m_trackNote = nullptr; // why a stream can't be picked / plays twice
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
