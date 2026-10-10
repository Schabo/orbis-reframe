#pragma once

#include <QWidget>

#include <cstdint>

class QAbstractButton;
class QLabel;
class QTimer;

// The playback control strip under the timeline: play/pause, trim in/out
// buttons, the reframe-authoring buttons, a screenshot button, an
// `mm:ss / mm:ss` label, a debounced "Buffering…" indicator and a transient
// notice line. The scrubbing / clip structure lives in TimelineWidget now.
//
// Pure view: it emits the user's intent and renders whatever state is pushed
// in through the setters. All times are milliseconds.
class TransportBar : public QWidget {
    Q_OBJECT

public:
    explicit TransportBar(QWidget *parent = nullptr);

public slots:
    void setDuration(qint64 durationMs);
    void setPosition(qint64 positionMs);
    void setPlaying(bool playing);
    void setBuffering(bool buffering);
    // Flips the arm toggle (the A key); emits reframeArmedChanged.
    void toggleReframeArmed();
    // Enables/disables the Del Key / Clr Path buttons.
    void setHasReframeKeys(bool hasKeys);
    // Enables/disables the screenshot button (off while one is rendering or
    // nothing is open).
    void setScreenshotEnabled(bool enabled);
    // A short status line beside the time label ("Saved …"), cleared after a
    // few seconds; `sticky` keeps it until the next notice.
    void showNotice(const QString &text, bool sticky = false);

signals:
    void playPauseClicked();
    // "Set in/out point at the playhead": no payload -- MainWindow uses the
    // current clip's source time (the transport bar only knows timeline time).
    void trimInRequested();
    void trimOutRequested();
    void trimCleared();
    // Reframe authoring: the arm toggle flipped; delete the key at the playhead;
    // clear the whole track.
    void reframeArmedChanged(bool armed);
    void deleteKeyRequested(); // at the playhead -- see trimInRequested()
    void clearReframeRequested();
    // Save the frame at the playhead as a PNG.
    void screenshotRequested();

private:
    void updateTimeLabel();

    QAbstractButton *m_playButton = nullptr;
    QAbstractButton *m_setInButton = nullptr;
    QAbstractButton *m_setOutButton = nullptr;
    QAbstractButton *m_clearTrimButton = nullptr;
    QAbstractButton *m_armReframeButton = nullptr;
    QAbstractButton *m_deleteKeyButton = nullptr;
    QAbstractButton *m_clearReframeButton = nullptr;
    QAbstractButton *m_screenshotButton = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_bufferingLabel = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QTimer *m_noticeClear = nullptr;
    // Delays showing the buffering label so a sub-second GetFrame() doesn't
    // make it flicker.
    QTimer *m_bufferingDelay = nullptr;

    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    bool m_playing = false;
};
