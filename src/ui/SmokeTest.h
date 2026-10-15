#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

class MainWindow;
class QTimer;
class QWidget;

// The end-to-end smoke test's hook: with ORBIS_SMOKE_TEST=<script> set, the
// app runs the script's steps against itself -- the real window, Vulkan
// viewport, decoder, tracker, plate scan and NVENC export -- and prints what
// it measured as "[smoke] name=value" lines on stdout, for a shell script to
// judge (tests/gpu/smoke_test.sh). Without the variable nothing of this is
// created.
//
// Driven the way a user drives the app, by name rather than by position:
// keys (the window's shortcuts), menu actions and buttons by objectName,
// dialogs by title. One step per line, '#' starts a comment; a step that
// waits gives up after its own timeout, records "<name>=timeout" and the
// script goes on. Steps:
//
//   wait-frame NAME SECONDS     until playback has handed the viewport a
//                               frame; NAME=ms since start
//   wait-drawn NAME SECONDS     until the viewport has drawn one; NAME=ms
//                               since start
//   key SEQUENCE                the window's shortcut for e.g. Space, T, Home
//   trigger ACTION              the QAction with that objectName
//   click BUTTON                the button with that objectName
//   sleep MS
//   play NAME MS                Space, MS of playback, Space; NAME_decoded /
//                               NAME_drawn = new frames handed to / drawn by
//                               the viewport, NAME_advance_ms = how far the
//                               playhead moved
//   grab NAME FILE              the viewport's picture to FILE (PNG);
//                               NAME_luma_mean, NAME_luma_stddev (0-255)
//   box X0 Y0 X1 Y1             a box drawn in the viewport, in fractions of
//                               it (after "key T": Track subject)
//   wait-track NAME SECONDS     until a Track subject run ends; NAME_samples,
//                               NAME_keys (the clip's reframe keys after),
//                               NAME_error
//   wait-scan NAME SECONDS      until a plate scan ends; NAME_frames,
//                               NAME_plates, NAME_error
//   dialog NAME TITLE SECONDS   until a modal dialog whose title matches the
//                               regular expression TITLE is up; NAME=its
//                               title; accepts it (a message box: its first
//                               accepting button)
//   save-as NAME SECONDS PATH   until a file dialog is up; types PATH, accepts
//   timeline NAME               NAME_ms = the timeline's length
//   quit                        quits (as File > Quit does)
//
// When the script ends without "quit", the app quits anyway.
class SmokeTest : public QObject
{
    Q_OBJECT

public:
    // Reads `scriptPath` and starts running it once the event loop runs.
    SmokeTest(MainWindow *window, const QString &scriptPath, QObject *parent = nullptr);

private:
    // One tick: runs the current step, or checks whether it's done.
    void tick();
    // The current step is done: on to the next one.
    void next();
    // Records name=value.
    void result(const QString &name, const QString &value);
    // The current step failed / timed out: records it, moves on.
    void fail(const QString &why);
    // The current step's timeout (its seconds argument) has passed.
    bool timedOut(int secondsArg) const;
    QWidget *modalTitled(const QString &pattern) const;

    MainWindow *m_window;
    QStringList m_steps;
    int m_step = -1;
    bool m_started = false; // the current step's start has run
    QElapsedTimer m_sinceStart;
    QElapsedTimer m_stepClock;
    QTimer *m_timer;
    // play: the viewport's frame counts and the playhead when playback began.
    int m_handedAtStart = 0;
    int m_takenAtStart = 0;
    qint64 m_playheadAtStart = 0;
    // Track subject runs and plate scans that have ended, with what the
    // last one found; wait-track / wait-scan take them one by one.
    struct JobEnd {
        int ended = 0;
        int taken = 0;
        int count = 0;  // samples tracked / frames scanned
        int plates = 0; // scans only
        QString error;
    };
    JobEnd m_track;
    JobEnd m_scan;
};
