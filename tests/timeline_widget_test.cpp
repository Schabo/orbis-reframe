// TimelineWidget's mouse handling (extra tracks), driven with QTest on the
// offscreen platform: audio clip select / move / cross-track move / trims,
// snapping, the track mute box, and that the main track still seeks and
// reorders; overlay (video) tracks above the main track. Exit code is
// the failure count. ORBIS_TIMELINE_SHOT=<path> saves a picture of the widget
// at the end.

#include "TimelineWidget.h"

#include <QApplication>
#include <QLineEdit>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QWheelEvent>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// Layout constants mirrored from TimelineWidget.cpp.
constexpr int kHeaderW = 180;
constexpr int kMarginX = 3;
constexpr int kRulerH = 14;
constexpr int kMainH = 40;
constexpr int kTrackH = 28;
constexpr int kRowGap = 2;
constexpr int kWidth = kHeaderW + 3 + 1000 + 3; // 1000 px for the extent: 1 px = extent / 1000

int trackY(int i) { return kRulerH + kMainH + kRowGap + i * (kTrackH + kRowGap) + kTrackH / 2; }
int mainY() { return kRulerH + kMainH / 2; }

struct Fixture {
    TimelineWidget w;
    Fixture()
    {
        // Main track: 10 s + 10 s. Audio: clip 100 on track 1 at 2..12 s, clip
        // 101 on track 2 at 15..20 s. The extent is 20 s: x = 183 + ms / 20.
        TimelineWidget::Clip a, b;
        a.id = 1;
        a.label = "a";
        a.trimOutMs = a.sourceDurationMs = 10000;
        b.id = 2;
        b.label = "b";
        b.trimOutMs = b.sourceDurationMs = 10000;
        w.setClips({a, b});
        TimelineWidget::TrackView t1, t2;
        t1.id = 11;
        t1.name = "Music";
        TimelineWidget::TrackClipView m;
        m.id = 100;
        m.label = "song";
        m.startMs = 2000;
        m.trimInMs = 1000;
        m.trimOutMs = 11000;
        m.sourceDurationMs = 60000;
        t1.clips = {m};
        t2.id = 12;
        t2.name = "Audio 2";
        TimelineWidget::TrackClipView n;
        n.id = 101;
        n.label = "fx";
        n.startMs = 15000;
        n.trimOutMs = n.sourceDurationMs = 5000;
        t2.clips = {n};
        w.setTracks({t1, t2});
        w.resize(kWidth, w.sizeHint().height());
        w.show();
        w.setPlayhead(7000);
    }
    static int x(qint64 ms) { return kHeaderW + kMarginX + int(ms / 20); }
};

void drag(QWidget *w, QPoint from, QPoint to)
{
    QTest::mousePress(w, Qt::LeftButton, Qt::NoModifier, from);
    const int steps = 8;
    for (int i = 1; i <= steps; ++i)
        QTest::mouseMove(w, from + (to - from) * i / steps);
    QTest::mouseRelease(w, Qt::LeftButton, Qt::NoModifier, to);
}

void testSelectAndMove()
{
    Fixture f;
    QSignalSpy sel(&f.w, &TimelineWidget::trackClipSelected);
    QSignalSpy seek(&f.w, &TimelineWidget::seekRequested);
    QSignalSpy moved(&f.w, &TimelineWidget::trackClipMoved);

    // A click selects without seeking.
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(Fixture::x(5000), trackY(0)));
    CHECK(sel.count() == 1 && sel.last().at(0).toInt() == 100);
    CHECK(seek.count() == 0 && moved.count() == 0);

    // Drag +1 s on the same track (away from any snap point).
    drag(&f.w, QPoint(Fixture::x(5000), trackY(0)), QPoint(Fixture::x(6000), trackY(0)));
    CHECK(moved.count() == 1);
    if (moved.count() == 1) {
        CHECK(moved.last().at(0).toInt() == 100 && moved.last().at(1).toInt() == 11);
        CHECK(std::abs(moved.last().at(2).toLongLong() - 3000) <= 20);
    }

    // Drag down onto track 2 by +0.5 s.
    drag(&f.w, QPoint(Fixture::x(5000), trackY(0)), QPoint(Fixture::x(5500), trackY(1)));
    CHECK(moved.count() == 2);
    if (moved.count() == 2) {
        CHECK(moved.last().at(1).toInt() == 12);
        CHECK(std::abs(moved.last().at(2).toLongLong() - 2500) <= 20);
    }

    // Snap: its start dropped 60 ms from the playhead (7 s) lands on it.
    drag(&f.w, QPoint(Fixture::x(5000), trackY(0)), QPoint(Fixture::x(9940), trackY(0)));
    CHECK(moved.count() == 3 && moved.last().at(2).toLongLong() == 7000);
    // ... and its end dropped near the main cut (10 s) lands on it: end =
    // start + 10 s, so start 0.
    drag(&f.w, QPoint(Fixture::x(5000), trackY(0)), QPoint(Fixture::x(3060), trackY(0)));
    CHECK(moved.count() == 4 && moved.last().at(2).toLongLong() == 0);

    // A click on empty track space clears the selection, without seeking.
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(Fixture::x(13000), trackY(0)));
    CHECK(sel.last().at(0).toInt() == -1);
    CHECK(seek.count() == 0);
}

void testLinkedStaysPut()
{
    // Clip 100 linked to a video clip: a drag can change its track, not its
    // time.
    Fixture f;
    TimelineWidget::TrackView t1, t2;
    t1.id = 11;
    t2.id = 12;
    TimelineWidget::TrackClipView m;
    m.id = 100;
    m.startMs = 2000;
    m.trimOutMs = m.sourceDurationMs = 10000;
    m.linked = true;
    t1.clips = {m};
    f.w.setTracks({t1, t2});
    QSignalSpy moved(&f.w, &TimelineWidget::trackClipMoved);
    drag(&f.w, QPoint(Fixture::x(5000), trackY(0)), QPoint(Fixture::x(8000), trackY(0)));
    CHECK(moved.count() == 0);
    drag(&f.w, QPoint(Fixture::x(5000), trackY(0)), QPoint(Fixture::x(8000), trackY(1)));
    CHECK(moved.count() == 1 && moved.last().at(1).toInt() == 12 &&
          moved.last().at(2).toLongLong() == 2000);
}

void testTrims()
{
    Fixture f;
    QSignalSpy in(&f.w, &TimelineWidget::trackClipTrimInRequested);
    QSignalSpy out(&f.w, &TimelineWidget::trackClipTrimOutRequested);

    // Left edge of clip 100 (at 2 s, trimIn 1 s) dragged right by 1.5 s.
    drag(&f.w, QPoint(Fixture::x(2000) + 1, trackY(0)), QPoint(Fixture::x(3500) + 1, trackY(0)));
    CHECK(in.count() == 1 && std::abs(in.last().at(1).toLongLong() - 2500) <= 20);
    // Left edge dragged far left: the start can't go below 0, so the in-point
    // stops 2 s earlier (at source 0... clamped to trimIn - start = -1 s => 0).
    drag(&f.w, QPoint(Fixture::x(2000) + 1, trackY(0)), QPoint(Fixture::x(0), trackY(0)));
    CHECK(in.count() == 2 && in.last().at(1).toLongLong() == 0);
    // Right edge (12 s) dragged to 14 s: trimOut 13 s.
    drag(&f.w, QPoint(Fixture::x(12000) - 1, trackY(0)), QPoint(Fixture::x(14000) - 1, trackY(0)));
    CHECK(out.count() == 1 && std::abs(out.last().at(1).toLongLong() - 13000) <= 40);
}

void testHeaderAndMain()
{
    Fixture f;
    QSignalSpy mute(&f.w, &TimelineWidget::trackMuteToggled);
    QSignalSpy seek(&f.w, &TimelineWidget::seekRequested);
    QSignalSpy reorder(&f.w, &TimelineWidget::clipMoved);

    // The "M" box sits at the right of the header.
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(kHeaderW - 13, trackY(1)));
    CHECK(mute.count() == 1 && mute.last().at(0).toInt() == 12 && mute.last().at(1).toBool());
    // The main video track has one too; it flips the state it was given.
    QSignalSpy mainMute(&f.w, &TimelineWidget::mainMuteToggled);
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(kHeaderW - 13, mainY()));
    CHECK(mainMute.count() == 1 && mainMute.last().at(0).toBool());
    f.w.setMainMuted(true);
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(kHeaderW - 13, mainY()));
    CHECK(mainMute.count() == 2 && !mainMute.last().at(0).toBool());
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(20, mainY())); // the name: nothing
    CHECK(mainMute.count() == 2 && mute.count() == 1);

    // Main track: a click selects that clip without seeking, a drag reorders.
    QSignalSpy sel(&f.w, &TimelineWidget::clipSelected);
    QSignalSpy scrub(&f.w, &TimelineWidget::scrubRequested);
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(Fixture::x(14000), mainY()));
    CHECK(seek.count() == 0 && scrub.count() == 0);
    CHECK(sel.count() == 1 && sel.last().at(0).toInt() == 2); // clip "b", not the playhead's
    // A click on an empty spot of a track row clears both selections.
    Fixture f2;
    QSignalSpy sel2(&f2.w, &TimelineWidget::clipSelected);
    QSignalSpy audioSel2(&f2.w, &TimelineWidget::trackClipSelected);
    QTest::mouseClick(&f2.w, Qt::LeftButton, Qt::NoModifier, QPoint(Fixture::x(13000), trackY(0)));
    CHECK(sel2.count() == 1 && sel2.last().at(0).toInt() == 0);
    CHECK(audioSel2.count() == 1 && audioSel2.last().at(0).toInt() == -1);
    // Only the ruler seeks. A click: one exact seek on press, no scrub.
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(Fixture::x(4000), kRulerH / 2));
    CHECK(seek.count() == 1 && std::abs(seek.last().at(0).toLongLong() - 4000) <= 20);
    CHECK(scrub.count() == 0);
    // A drag: an exact seek on press, scrubs while it moves, one on release.
    drag(&f.w, QPoint(Fixture::x(1000), kRulerH / 2), QPoint(Fixture::x(6000), kRulerH / 2));
    CHECK(seek.count() == 3 && std::abs(seek.at(1).at(0).toLongLong() - 1000) <= 20 &&
          std::abs(seek.last().at(0).toLongLong() - 6000) <= 20);
    CHECK(scrub.count() > 2);
    // A drag across a track row doesn't scrub.
    const int scrubs = scrub.count();
    drag(&f.w, QPoint(Fixture::x(13000), trackY(0)), QPoint(Fixture::x(14000), trackY(0)));
    CHECK(scrub.count() == scrubs && seek.count() == 3);
    drag(&f.w, QPoint(Fixture::x(3000), mainY()), QPoint(Fixture::x(19000), mainY()));
    CHECK(reorder.count() == 1 && reorder.last().at(0).toInt() == 1 &&
          reorder.last().at(1).toInt() == 1);

    // The widget grows a row per track.
    const int h2 = f.w.sizeHint().height();
    f.w.setTracks({});
    CHECK(f.w.sizeHint().height() == h2 - 2 * (kTrackH + kRowGap));

    if (qEnvironmentVariableIsSet("ORBIS_TIMELINE_SHOT")) {
        Fixture g;
        // Fake waveforms: a swell on the audio clips, a noisy bed on the video.
        auto wave = [](int n, double level) {
            auto v = std::make_shared<std::vector<float>>(n);
            for (int i = 0; i < n; ++i)
                (*v)[i] = float(level * (0.3 + 0.7 * std::abs(std::sin(i / 40.0))) *
                                (0.6 + 0.4 * ((i * 7919) % 13) / 13.0));
            return TimelineWidget::Peaks(v);
        };
        TimelineWidget::Clip a, b;
        a.id = 1;
        a.label = "a";
        a.trimOutMs = a.sourceDurationMs = 10000;
        a.peaks = wave(1000, 0.2);
        b = a;
        b.id = 2;
        b.label = "b";
        g.w.setClips({a, b});
        TimelineWidget::TrackView t1, t2;
        t1.id = 11;
        t1.name = "Music";
        TimelineWidget::TrackClipView m;
        m.id = 100;
        m.label = "song";
        m.startMs = 2000;
        m.trimOutMs = 10000;
        m.sourceDurationMs = 60000;
        m.fadeInMs = 2000;
        m.fadeOutMs = 1000;
        m.peaks = wave(6000, 0.8);
        t1.clips = {m};
        t2.id = 12;
        t2.name = "Audio 2";
        TimelineWidget::TrackClipView n = m;
        n.id = 101;
        n.label = "fx (-12 dB)";
        n.startMs = 15000;
        n.trimOutMs = 5000;
        n.fadeInMs = n.fadeOutMs = 0;
        n.gain = 0.25;
        t2.clips = {n};
        g.w.setTracks({t1, t2});
        g.w.setActiveClip(1); // under the playhead, but an audio clip is selected
        g.w.setSelectedTrackClip(100);
        g.w.grab().save(qEnvironmentVariable("ORBIS_TIMELINE_SHOT"));
        for (int i = 0; i < 8; ++i) // and zoomed in around the playhead
            g.w.zoomIn();
        g.w.grab().save(qEnvironmentVariable("ORBIS_TIMELINE_SHOT").replace(".png", "-zoom.png"));
    }
}

void wheel(QWidget *w, QPoint at, int delta, Qt::KeyboardModifiers mods)
{
    QWheelEvent ev(QPointF(at), QPointF(w->mapToGlobal(at)), QPoint(), QPoint(0, delta),
                   Qt::NoButton, mods, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &ev);
}

// Where a ruler click at x seeks to.
qint64 rulerMs(QWidget *w, int x)
{
    QSignalSpy seek(w, SIGNAL(seekRequested(qint64)));
    QTest::mouseClick(w, Qt::LeftButton, Qt::NoModifier, QPoint(x, kRulerH / 2));
    return seek.count() == 1 ? seek.last().at(0).toLongLong() : -1;
}

void testZoom()
{
    Fixture f; // extent 20 s over 1000 px: 20 ms / px
    const int x10 = Fixture::x(10000);
    CHECK(std::abs(rulerMs(&f.w, x10) - 10000) <= 20);

    // Ctrl+wheel one notch in, anchored at 10 s: 16 ms / px, 10 s stays put.
    wheel(&f.w, QPoint(x10, trackY(0)), 120, Qt::ControlModifier);
    CHECK(std::abs(rulerMs(&f.w, x10) - 10000) <= 20);
    CHECK(std::abs(rulerMs(&f.w, x10 + 100) - 11600) <= 20);

    // Plain wheel pans a tenth of the view (16 s) per notch: 1.6 s later.
    wheel(&f.w, QPoint(x10, trackY(0)), -120, Qt::NoModifier);
    CHECK(std::abs(rulerMs(&f.w, x10) - 11600) <= 20);

    // The view can't pan past the end: 16 s shown of 20, so it starts <= 4 s.
    for (int i = 0; i < 10; ++i)
        wheel(&f.w, QPoint(x10, trackY(0)), -120, Qt::NoModifier);
    CHECK(std::abs(rulerMs(&f.w, Fixture::x(0)) - 4000) <= 20);

    // Zoomed right in: capped at 1 s across the view.
    for (int i = 0; i < 40; ++i)
        f.w.zoomIn();
    CHECK(std::abs((rulerMs(&f.w, Fixture::x(20000)) - rulerMs(&f.w, Fixture::x(0))) - 1000) <= 5);

    // The view follows the playhead once it runs off screen (playback).
    f.w.setPlayhead(1000);
    const qint64 left = rulerMs(&f.w, Fixture::x(0));
    CHECK(left <= 1000 && left > 0); // paged so it's back in view, near the left

    // Fit: back to the original mapping.
    f.w.zoomToFit();
    CHECK(std::abs(rulerMs(&f.w, x10) - 10000) <= 20);
    // Zooming out past the whole is just fit.
    wheel(&f.w, QPoint(x10, trackY(0)), -120 * 5, Qt::ControlModifier);
    CHECK(std::abs(rulerMs(&f.w, x10) - 10000) <= 20);
}

void testTrackHeaders()
{
    Fixture f;
    QSignalSpy moved(&f.w, &TimelineWidget::trackMoved);
    QSignalSpy renamed(&f.w, &TimelineWidget::trackRenameRequested);

    // Drag track 1's header below track 2.
    drag(&f.w, QPoint(20, trackY(0)), QPoint(20, trackY(1) + 10));
    CHECK(moved.count() == 1 && moved.last().at(0).toInt() == 11 && moved.last().at(1).toInt() == 1);
    // ... and a drag that ends where it started does nothing.
    drag(&f.w, QPoint(20, trackY(1)), QPoint(20, trackY(1) + 3));
    CHECK(moved.count() == 1);

    // Double-click the header: an edit box; typing + Enter renames.
    QTest::mouseDClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(20, trackY(1)));
    auto *edit = f.w.findChild<QLineEdit *>();
    CHECK(edit && edit->isVisible() && edit->text() == "Audio 2");
    if (edit) {
        QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(edit, "Drums");
        QTest::keyClick(edit, Qt::Key_Return);
        CHECK(renamed.count() == 1 && renamed.last().at(0).toInt() == 12 &&
              renamed.last().at(1).toString() == "Drums");
        CHECK(!edit->isVisible());
        // Escape cancels.
        QTest::mouseDClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(20, trackY(0)));
        QTest::keyClicks(edit, "xx");
        QTest::keyClick(edit, Qt::Key_Escape);
        CHECK(renamed.count() == 1 && !edit->isVisible());
    }
}

// Drag `files` over the widget to `at` and drop them; returns whether the
// drag was accepted on entry.
bool dropFiles(QWidget *w, const QStringList &files, QPoint at)
{
    QMimeData mime;
    QList<QUrl> urls;
    for (const QString &f : files)
        urls << QUrl::fromLocalFile(f);
    mime.setUrls(urls);
    QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &enter);
    if (!enter.isAccepted())
        return false;
    QDragMoveEvent move(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
    QDropEvent drop(QPointF(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &drop);
    return true;
}

void testDrop()
{
    Fixture f;
    QSignalSpy dropped(&f.w, &TimelineWidget::audioFilesDropped);

    // Onto the second audio track at 5 s: that track, that time.
    CHECK(dropFiles(&f.w, {"/x/a.mp3", "/x/b.WAV", "/x/notes.txt"}, QPoint(Fixture::x(5000), trackY(1))));
    CHECK(dropped.count() == 1);
    if (dropped.count() == 1) {
        const QStringList paths = dropped.last().at(0).toStringList();
        CHECK(paths == QStringList({"/x/a.mp3", "/x/b.WAV"})); // the .txt is left out
        CHECK(dropped.last().at(1).toInt() == 12);
        CHECK(std::abs(dropped.last().at(2).toLongLong() - 5000) <= 20);
    }
    // Onto the video row: no track given (first with room / a new one); near
    // the playhead it snaps to it.
    CHECK(dropFiles(&f.w, {"/x/a.flac"}, QPoint(Fixture::x(7060), mainY())));
    CHECK(dropped.count() == 2 && dropped.last().at(1).toInt() == 0 &&
          dropped.last().at(2).toLongLong() == 7000);
    // Nothing it can take: refused.
    CHECK(!dropFiles(&f.w, {"/x/clip.lrv", "/x/a.txt"}, QPoint(Fixture::x(5000), trackY(0))));
    // Nothing on the video track: nothing to lay audio under.
    TimelineWidget empty;
    empty.resize(kWidth, empty.sizeHint().height());
    CHECK(!dropFiles(&empty, {"/x/a.mp3"}, QPoint(Fixture::x(0), mainY())));
}

void testDropClips()
{
    Fixture f;
    QSignalSpy clips(&f.w, &TimelineWidget::clipFilesDropped);
    QSignalSpy audio(&f.w, &TimelineWidget::audioFilesDropped);

    // At the cut nearest the pointer, wherever it's dropped: 7 s is past the
    // middle of the first clip -> between the two (index 1).
    CHECK(dropFiles(&f.w, {"/x/a.insv", "/x/b.INSV", "/x/a.lrv"}, QPoint(Fixture::x(7000), trackY(0))));
    CHECK(clips.count() == 1 && audio.count() == 0);
    if (clips.count() == 1) {
        CHECK(clips.last().at(0).toStringList() == QStringList({"/x/a.insv", "/x/b.INSV"}));
        CHECK(clips.last().at(1).toInt() == 1);
    }
    CHECK(dropFiles(&f.w, {"/x/a.insv"}, QPoint(Fixture::x(3000), mainY())));
    CHECK(clips.count() == 2 && clips.last().at(1).toInt() == 0);
    CHECK(dropFiles(&f.w, {"/x/a.insv"}, QPoint(Fixture::x(19000), mainY())));
    CHECK(clips.count() == 3 && clips.last().at(1).toInt() == 2);
    // With audio: the clips first, the audio where they start.
    CHECK(dropFiles(&f.w, {"/x/a.insv", "/x/m.mp3"}, QPoint(Fixture::x(12000), mainY())));
    CHECK(clips.count() == 4 && clips.last().at(1).toInt() == 1);
    CHECK(audio.count() == 1 && audio.last().at(1).toInt() == 0 &&
          audio.last().at(2).toLongLong() == 10000);

    // An empty timeline takes clips (and nothing else).
    TimelineWidget empty;
    empty.resize(kWidth, empty.sizeHint().height());
    QSignalSpy first(&empty, &TimelineWidget::clipFilesDropped);
    CHECK(dropFiles(&empty, {"/x/a.insv"}, QPoint(Fixture::x(500), mainY())));
    CHECK(first.count() == 1 && first.last().at(1).toInt() == 0);
}


// Overlay tracks sit above the main track; their clips move only among
// overlay tracks, and video files drop onto them.
void testOverlayTracks()
{
    Fixture f;
    TimelineWidget::TrackView v1, v2, a1;
    v1.id = 21;
    v1.name = "Cam";
    v1.video = true;
    TimelineWidget::TrackClipView pip;
    pip.id = 200;
    pip.label = "pip.mp4";
    pip.startMs = 4000;
    pip.trimOutMs = pip.sourceDurationMs = 6000;
    pip.keySourceMs = {1000, 3000};
    v1.clips = {pip};
    v2.id = 22;
    v2.name = "Overlay 2";
    v2.video = true;
    a1.id = 11;
    a1.name = "Music";
    TimelineWidget::TrackClipView m;
    m.id = 100;
    m.label = "song";
    m.startMs = 2000;
    m.trimOutMs = m.sourceDurationMs = 5000;
    a1.clips = {m};
    f.w.setTracks({v1, v2, a1});
    f.w.resize(kWidth, f.w.sizeHint().height());

    // Rows: Cam, Overlay 2, then the main track, then Music.
    const int camY = kRulerH + kTrackH / 2;
    const int ov2Y = kRulerH + (kTrackH + kRowGap) + kTrackH / 2;
    const int mainRowY = kRulerH + 2 * (kTrackH + kRowGap) + kMainH / 2;
    const int musicY = kRulerH + 2 * (kTrackH + kRowGap) + kMainH + kRowGap + kTrackH / 2;
    CHECK(f.w.sizeHint().height() > musicY + kTrackH / 2);

    QSignalSpy sel(&f.w, &TimelineWidget::trackClipSelected);
    QSignalSpy clipSel(&f.w, &TimelineWidget::clipSelected);
    QSignalSpy moved(&f.w, &TimelineWidget::trackClipMoved);
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(Fixture::x(6000), camY));
    CHECK(sel.count() == 1 && sel.last().at(0).toInt() == 200);
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(Fixture::x(6000), mainRowY));
    CHECK(clipSel.count() >= 1 && clipSel.last().at(0).toInt() == 1);
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(Fixture::x(3000), musicY));
    CHECK(sel.last().at(0).toInt() == 100);

    // An overlay clip moves onto the other overlay track...
    drag(&f.w, QPoint(Fixture::x(6000), camY), QPoint(Fixture::x(6500), ov2Y));
    CHECK(moved.count() == 1 && moved.last().at(1).toInt() == 22);
    // ... but never onto the audio track (it keeps the last overlay row it
    // passed over), nor does the audio clip go onto an overlay track.
    drag(&f.w, QPoint(Fixture::x(6000), camY), QPoint(Fixture::x(6500), musicY));
    CHECK(moved.count() == 2 && moved.last().at(1).toInt() == 22);
    drag(&f.w, QPoint(Fixture::x(3000), musicY), QPoint(Fixture::x(3500), camY));
    CHECK(moved.count() == 3 && moved.last().at(1).toInt() == 11);

    // A header drag keeps an overlay track among the overlay tracks.
    QSignalSpy trackMoved(&f.w, &TimelineWidget::trackMoved);
    drag(&f.w, QPoint(20, camY), QPoint(20, musicY + kTrackH));
    CHECK(trackMoved.count() == 1 && trackMoved.last().at(1).toInt() == 1);

    // The "H" box hides an overlay track.
    QSignalSpy mute(&f.w, &TimelineWidget::trackMuteToggled);
    QTest::mouseClick(&f.w, Qt::LeftButton, Qt::NoModifier, QPoint(kHeaderW - 12, camY));
    CHECK(mute.count() == 1 && mute.last().at(0).toInt() == 21);

    // Video files drop onto an overlay track; onto an audio track or the
    // main track they go to the first overlay track with room (0).
    QSignalSpy videoDrop(&f.w, &TimelineWidget::videoFilesDropped);
    QSignalSpy audioDrop(&f.w, &TimelineWidget::audioFilesDropped);
    CHECK(dropFiles(&f.w, {"/x/cam.MP4", "/x/b.mov"}, QPoint(Fixture::x(12000), ov2Y)));
    CHECK(videoDrop.count() == 1 && audioDrop.count() == 0);
    if (videoDrop.count() == 1) {
        CHECK(videoDrop.last().at(0).toStringList() == QStringList({"/x/cam.MP4", "/x/b.mov"}));
        CHECK(videoDrop.last().at(1).toInt() == 22);
    }
    CHECK(dropFiles(&f.w, {"/x/cam.mp4"}, QPoint(Fixture::x(12000), musicY)));
    CHECK(videoDrop.count() == 2 && videoDrop.last().at(1).toInt() == 0);
    // Audio dropped on an overlay track goes to an audio track.
    CHECK(dropFiles(&f.w, {"/x/a.mp3"}, QPoint(Fixture::x(12000), camY)));
    CHECK(audioDrop.count() == 1 && audioDrop.last().at(1).toInt() == 0);

    if (const char *shot = std::getenv("ORBIS_TIMELINE_SHOT"))
        f.w.grab().save(QString::fromLocal8Bit(shot));
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testSelectAndMove();
    testLinkedStaysPut();
    testTrims();
    testHeaderAndMain();
    testZoom();
    testTrackHeaders();
    testDrop();
    testDropClips();
    testOverlayTracks();
    if (g_failures == 0)
        std::printf("timeline_widget_test: all checks passed\n");
    else
        std::printf("timeline_widget_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
