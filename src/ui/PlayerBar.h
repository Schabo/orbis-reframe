#pragma once

#include <QList>
#include <QWidget>

#include <optional>

class QAbstractButton;
class QLabel;
class QLineEdit;
class QSlider;
class QTimer;
class QToolButton;

// The scrub strip in the player bar: the whole project end to end, whatever
// the timeline's zoom. Played part in the accent, the cuts as small ticks,
// the time under the mouse as a tooltip. Scrubs like the timeline's ruler:
// seekRequested (the exact frame) on press, scrubRequested while dragging,
// seekRequested again on release after a drag.
class PlayerProgress : public QWidget {
    Q_OBJECT

public:
    explicit PlayerProgress(QWidget *parent = nullptr);

    void setDuration(qint64 durationMs);
    void setPosition(qint64 positionMs);
    // The cuts between clips, timeline ms (neither 0 nor the end).
    void setCuts(const QList<qint64> &cutsMs);
    // For the hover tooltip: how times are written (PlayerBar::formatTime).
    void setFrameRate(double fps) { m_fps = fps; }

    // The timeline time at x (widget coordinates), clamped to the project.
    qint64 timeAt(int x) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void seekRequested(qint64 timelineMs);
    void scrubRequested(qint64 timelineMs);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect grooveRect() const;
    int xAt(qint64 ms) const;

    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    QList<qint64> m_cutsMs;
    double m_fps = 0.0;
    bool m_pressed = false;
    bool m_dragMoved = false;
    int m_pressX = 0;
    bool m_hover = false;
};

// The player bar directly under the viewport: playback only. Two rows -- on
// top the time, the progress strip over the whole project and the playback
// speed; below "Buffering…", the playback buttons (to start, previous cut,
// one frame back, play / pause, one frame forward, next cut, to end), then
// screenshot, preview volume and full screen.
//
// Pure view, like TransportBar: it emits the user's intent and shows what
// the setters push in. Times are timeline milliseconds. Every widget has an
// objectName (playerPlayButton, playerProgress, playerTime, …).
class PlayerBar : public QWidget {
    Q_OBJECT

public:
    explicit PlayerBar(QWidget *parent = nullptr);

    // The speeds offered, slowest first; 1 is among them.
    static const QList<double> &speeds();
    // `ms` as hours:minutes:seconds:frames at `fps` ("00:01:23:14"), or with
    // hundredths ("00:01:23.45") without a frame rate.
    static QString formatTime(qint64 ms, double fps);
    // A typed time: hours:minutes:seconds:frames (as shown), or shorter --
    // [[hours:]minutes:]seconds, the seconds with a decimal part if no frames
    // ("1:23.5"). Frames need a frame rate. Empty when it can't be read.
    static std::optional<qint64> parseTime(const QString &text, double fps);

    double speed() const { return m_speed; }
    int volume() const;
    bool isMuted() const { return m_muted; }

public slots:
    void setDuration(qint64 durationMs);
    void setPosition(qint64 positionMs);
    void setPlaying(bool playing);
    void setBuffering(bool buffering);
    // The rate the time display counts frames in (the project's output rate);
    // 0 shows hundredths.
    void setFrameRate(double fps);
    void setCuts(const QList<qint64> &cutsMs);
    void setScreenshotEnabled(bool enabled);
    // Picks the nearest offered speed; emits speedChanged when it changes.
    void setSpeed(double speed);
    // One step slower (-1) / faster (+1) through speeds(), stopping at the
    // ends.
    void stepSpeed(int direction);
    // 0..100; emits volumeChanged when it changes.
    void setVolume(int percent);
    void setMuted(bool muted);
    // Only the button's look (maximize / minimize): MainWindow owns the mode.
    void setFullScreen(bool fullScreen);
    // Click-to-type on the time display, as a click on it does.
    void editTime();

signals:
    void playPauseClicked();
    void toStartClicked();
    void toEndClicked();
    void previousCutClicked();
    void nextCutClicked();
    void frameBackClicked();
    void frameForwardClicked();
    // The progress strip, and a time typed into the time display (seek only).
    void seekRequested(qint64 timelineMs);
    void scrubRequested(qint64 timelineMs);
    void speedChanged(double speed);
    void volumeChanged(int percent);
    void mutedChanged(bool muted);
    void screenshotRequested();
    void fullScreenClicked();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateTime();
    void updateVolumeIcon();
    void finishTimeEdit(bool apply);

    PlayerProgress *m_progress = nullptr;
    QToolButton *m_timeButton = nullptr;
    QLineEdit *m_timeEdit = nullptr;
    QToolButton *m_speedButton = nullptr;
    QAbstractButton *m_playButton = nullptr;
    QAbstractButton *m_screenshotButton = nullptr;
    QToolButton *m_muteButton = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QToolButton *m_fullScreenButton = nullptr;
    QLabel *m_bufferingLabel = nullptr;
    // Delays showing the buffering label so a sub-second GetFrame() doesn't
    // make it flicker.
    QTimer *m_bufferingDelay = nullptr;

    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    double m_fps = 0.0;
    double m_speed = 1.0;
    bool m_muted = false;
};
