#pragma once

#include "../SettingsPanel.h"

#include <QList>
#include <QObject>

#include <NoiseReduction.h> // insv::NoiseReduction

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QScrollArea;
class QSlider;
class QVBoxLayout;

// What the Audio tab shows of one audio clip.
struct AudioClipState {
    int clipId = 0;  // TrackClip::id
    int trackId = 0; // Track::id
    // Which kind of track, for the section's name (its open / closed state
    // is remembered per kind): "cameraMic", "externalMic" or "clip".
    QString kind = QStringLiteral("clip");
    QString label; // the clip's file name
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

// One audio clip's section of the Audio tab, titled by its track: which
// clip, its noise reduction, its track's level and mute, its own, then its
// fades and start. Edits come back with the clip's (or track's) id.
class AudioClipSection : public SettingsPanel {
    Q_OBJECT

public:
    explicit AudioClipSection(QWidget *parent = nullptr);

    int clipId() const { return m_state.clipId; }
    // Does not emit.
    void setState(const AudioClipState &s);

signals:
    void clipEdited(int clipId, double gainDb, bool muted, qint64 fadeInMs, qint64 fadeOutMs,
                    qint64 startMs);
    void noiseEdited(int clipId, const insv::NoiseReduction &noise);
    void trackLevelEdited(int trackId, double gainDb, bool muted);

private:
    void setNoise(const insv::NoiseReduction &n);
    void emitIfEdited();
    void emitTrackIfEdited();
    void emitNoiseIfEdited();

    AudioClipState m_state;
    QLabel *m_clipName = nullptr;
    QComboBox *m_noiseMode = nullptr;
    int m_lastNoiseIndex = 0; // to go back to when a voice mode can't run
    QSlider *m_noiseStrength = nullptr; // percent
    QLabel *m_noiseStrengthValue = nullptr;
    QSlider *m_trackGain = nullptr;
    QLabel *m_trackGainValue = nullptr;
    QCheckBox *m_trackMute = nullptr;
    QSlider *m_gain = nullptr;
    QLabel *m_gainValue = nullptr;
    QCheckBox *m_mute = nullptr;
    QDoubleSpinBox *m_fadeIn = nullptr;
    QDoubleSpinBox *m_fadeOut = nullptr;
    QDoubleSpinBox *m_start = nullptr;
    bool m_updating = false;
};

// The Audio tab: the audio clip selected on the timeline, or else every
// audio clip linked to the selected overlay clip, or to the video clip
// that's selected or under the playhead (its camera mic, its external mic,
// ...: neither has sound of its own) -- one AudioClipSection each, top to bottom in track order. With none,
// a greyed placeholder section says what to select.
//
// Not a widget itself: it fills the sidebar's Audio page (attach()) and
// rebuilds its sections only when the set of clips changes -- otherwise it
// updates them in place, so an edit in progress keeps its widgets. Open /
// closed is remembered per kind of track (QSettings, like the other
// sections). Nothing here talks to AudioPlayer; it all applies to playback
// and export alike.
class AudioPanel : public QObject {
    Q_OBJECT

public:
    explicit AudioPanel(QObject *parent = nullptr);

    // The page to fill (makeSettingsPage's, with no sections of its own).
    void attach(QScrollArea *page);

    // `scope`: what they are ("This clip", "Selected audio clip", "Selected
    // overlay clip"), shown on each section. No clips with a scope: the clip
    // it names has no sound, and the placeholder says so. Does not emit.
    void setClips(const QList<AudioClipState> &clips, const QString &scope);
    // Nothing to show: the placeholder.
    void setNoClip();

    // The sections showing now (the placeholder when there are no clips).
    QList<SettingsPanel *> sections() const;
    // What the sections apply to ("This clip" / "Selected audio clip"), or
    // empty with no clip.
    QString scope() const;

signals:
    void audioClipEdited(int clipId, double gainDb, bool muted, qint64 fadeInMs,
                         qint64 fadeOutMs, qint64 startMs);
    void noiseEdited(int clipId, const insv::NoiseReduction &noise);
    void trackLevelEdited(int trackId, double gainDb, bool muted);

private:
    void clearSections();
    // The placeholder: what to select, or (`scope` set) that clip has no sound.
    void showPlaceholder(const QString &scope);

    QVBoxLayout *m_layout = nullptr; // the page's column of sections
    SettingsPanel *m_placeholder = nullptr;
    QString m_placeholderScope; // the placeholder's: empty = no clip
    QList<AudioClipSection *> m_sections;
    QString m_scope;
};
