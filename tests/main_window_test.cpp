// The whole main window on the offscreen platform, without a GPU: no Vulkan
// instance (the viewport keeps its state but never renders) and stand-ins
// for the clip probe and the stitch source (ClipBackend), so "camera files"
// are any file named *.insv and frames are flat grey; audio and overlay
// files are generated (support/TestMedia). Driven like a user would: menu
// actions and buttons found by objectName, keys, file dialogs and message
// boxes answered from a timer, and the timeline / media pool's signals (their
// own mouse handling has its own tests). Covers the start screen, opening and
// adding clips, playback across a cut, edits by key and their undo, the media
// pool, audio and overlay tracks, the Colour section, the View and Help
// menus, Save as, autosave recovery, locating a moved file, and closing with
// unsaved changes; also full screen, overlay layers, Track subject's box
// mode and the menu entries that ask for files. Exit code is the failure
// count.

#include "ClipBackend.h"
#include "MainWindow.h"
#include "MediaPoolPanel.h"
#include "Theme.h"
#include "TimelineWidget.h"
#include "support/TestMedia.h"

#include "../core/Autosave.h"
#include "../core/MediaPool.h"
#include "../core/Project.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <set>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

template <typename T> T *find(QObject *root, const char *name)
{
    T *w = root->findChild<T *>(QLatin1String(name));
    if (!w)
        std::printf("FAIL  no %s named \"%s\"\n", T::staticMetaObject.className(), name);
    return w;
}

// Every stand-in clip: 4 s of 30 fps, the X5's 8K sphere.
constexpr int64_t kClipMs = 4000;
constexpr double kFps = 30.0;

std::atomic<int> g_framesServed{0};
std::mutex g_openedMutex;
std::set<std::string> g_opened; // paths the stand-in source was opened on

bool wasOpened(const QString &path)
{
    std::lock_guard<std::mutex> lock(g_openedMutex);
    return g_opened.count(path.toStdString()) > 0;
}

// Flat grey equirect frames, small, one per 1/30 s.
class StandInSource : public stitch::IStitchSource {
public:
    bool Open(const std::string &path, const stitch::PreviewStitchConfig &) override
    {
        if (!QFile::exists(QString::fromStdString(path))) {
            m_error = "no such file";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_openedMutex);
            g_opened.insert(path);
        }
        m_info = {7680, 3840, kFps, kClipMs};
        return true;
    }
    const stitch::ClipInfo &GetInfo() const override { return m_info; }
    stitch::Frame GetFrame(int64_t ms) override
    {
        SeekTo(ms);
        return ReadFrame();
    }
    bool SeekTo(int64_t ms) override
    {
        m_frame = static_cast<int64_t>(ms * kFps / 1000.0 + 0.5);
        return true;
    }
    stitch::Frame ReadFrame() override
    {
        stitch::Frame f;
        const int64_t ms = static_cast<int64_t>(m_frame * 1000.0 / kFps);
        if (ms >= m_info.duration_ms)
            return f;
        f.width = 64;
        f.height = 32;
        f.rgba.assign(size_t(f.width) * f.height * 4, 128);
        f.timestamp_ms = ms;
        ++m_frame;
        ++g_framesServed;
        return f;
    }
    const std::string &GetLastError() const override { return m_error; }

private:
    stitch::ClipInfo m_info;
    int64_t m_frame = 0;
    std::string m_error;
};

ClipBackend standIns()
{
    ClipBackend b;
    b.probe = [](const std::string &path, stitch::ClipProbe &out, std::string &err) {
        if (!QFile::exists(QString::fromStdString(path))) {
            err = "no such file";
            return false;
        }
        out = {7680, 3840, kFps, kClipMs};
        return true;
    };
    b.audioStreamCount = [](const std::string &) { return 0; };
    b.makeSource = [] { return std::make_unique<StandInSource>(); };
    b.measure = false;
    return b;
}

void writeFile(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write("not really a camera file");
}

// Runs the event loop until `done` (called once per turn: it may consume
// what it checks) or `ms` pass.
template <typename F> bool waitFor(F done, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() >= ms)
            return false;
        QTest::qWait(10);
    }
    return true;
}

// Answers the next modal dialog (a file dialog, a message box, one of ours)
// with `fn` once it is up -- the code that opens it is blocked in exec().
// Gives up after a few seconds, so a dialog that never comes fails the check
// that follows instead of hanging the test.
void whenDialog(std::function<void(QWidget *)> fn)
{
    auto *timer = new QTimer;
    auto *elapsed = new QElapsedTimer;
    elapsed->start();
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, [timer, elapsed, fn] {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal && elapsed->elapsed() < 5000)
            return;
        timer->stop();
        timer->deleteLater();
        delete elapsed;
        if (modal) {
            fn(modal);
        } else {
            std::printf("FAIL  no dialog came up\n");
            ++g_failures;
        }
    });
    timer->start();
}

void answerFileDialog(const QString &path)
{
    whenDialog([path](QWidget *w) {
        auto *dialog = qobject_cast<QFileDialog *>(w);
        CHECK(dialog);
        if (!dialog) {
            w->close();
            return;
        }
        // Typed into the file name field: selectFile() is lost while the
        // dialog's directory listing is still loading.
        auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
        CHECK(name);
        if (name)
            name->setText(path);
        static_cast<QDialog *>(dialog)->accept(); // QFileDialog's own is protected
    });
}

// Presses the message box's button whose text has `text` (mnemonics ignored).
void answerMessageBox(const QString &text)
{
    whenDialog([text](QWidget *w) {
        auto *box = qobject_cast<QMessageBox *>(w);
        CHECK(box);
        if (!box) {
            w->close();
            return;
        }
        for (QAbstractButton *b : box->buttons())
            if (b->text().remove(QLatin1Char('&')) == text) {
                b->click();
                return;
            }
        std::printf("FAIL  no \"%s\" button in \"%s\"\n", qPrintable(text),
                    qPrintable(box->text()));
        ++g_failures;
        box->reject();
    });
}

struct Ctx {
    MainWindow *w = nullptr;
    Project *project = nullptr;
    Timeline *timeline = nullptr;
    TimelineWidget *timelineWidget = nullptr;
    QString dir;
    QString clipA, clipB, wav, mp4, orp;
};

QAction *action(const Ctx &c, const char *name) { return find<QAction>(c.w, name); }

void trigger(const Ctx &c, const char *name)
{
    if (QAction *a = action(c, name))
        a->trigger();
}

// A key press on the window, through the window system's path like a real
// one (QShortcuts included).
void key(const Ctx &c, int k, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    // A closed dialog doesn't hand activation back on the offscreen platform.
    if (!c.w->isActiveWindow()) {
        c.w->activateWindow();
        if (!QTest::qWaitForWindowActive(c.w))
            std::printf("      (the window didn't become active)\n");
    }
    QTest::keyClick(c.w->windowHandle(), Qt::Key(k), mods);
    QApplication::processEvents();
}

// Moves the playhead (as a click on the ruler does) and waits for it.
bool seekTo(const Ctx &c, qint64 ms)
{
    emit c.timelineWidget->seekRequested(ms);
    return waitFor([&] { return c.timelineWidget->playhead() == ms; });
}

// Edits closer together than this are one undo step (Project's
// kUndoCoalesceMs); waited out between edits meant to be undone one by one.
void settle() { QTest::qWait(700); }

int trackIdWhere(const Ctx &c, const std::function<bool(const Track &)> &pred)
{
    for (int i = 0; i < c.timeline->trackCount(); ++i)
        if (pred(c.timeline->trackAt(i)))
            return c.timeline->trackAt(i).id;
    return 0;
}

const Track *track(const Ctx &c, int id) { return c.timeline->trackById(id); }

// --- the sections, in the order main() runs them --------------------------

void testStartScreen(const Ctx &c)
{
    auto *pages = c.w->findChild<QStackedWidget *>();
    CHECK(pages);
    CHECK(pages && pages->currentWidget() == c.w->findChild<QWidget *>(QStringLiteral("projectLibrary")));
    // Editor-only actions wait for a project.
    if (QAction *a = action(c, "saveProjectAction"))
        CHECK(!a->isEnabled());
    if (QAction *a = action(c, "addAudioTrackAction"))
        CHECK(!a->isEnabled());
}

void testOpenClip(const Ctx &c)
{
    c.w->openClip(c.clipA);
    auto *pages = c.w->findChild<QStackedWidget *>();
    CHECK(pages && pages->currentWidget() == find<QSplitter>(c.w, "editorSplitter"));
    CHECK(c.timeline->count() == 1);
    if (c.timeline->count() == 1)
        CHECK(c.timeline->clipAt(0).sourceDurationMs == kClipMs);
    if (QAction *a = action(c, "saveProjectAction"))
        CHECK(a->isEnabled());
    // Whatever goes on the timeline goes into the media pool too.
    CHECK(c.project->mediaPool()->contains(c.clipA));
}

// The stand-in source is opened on the playback thread and its frames reach
// the viewport, which keeps the latest one without a renderer to take it.
void testPlayback(const Ctx &c)
{
    EquirectViewportWindow *viewport = c.w->viewport();
    CHECK(waitFor([] { return g_framesServed.load() > 0; }));
    auto *play = find<QToolButton>(c.w, "playerPlayButton");
    auto *time = find<QToolButton>(c.w, "playerTime");
    if (!play || !time)
        return;
    const QString start = time->text();
    play->click();
    CHECK(waitFor([&] { return time->text() != start; })); // the playhead moves
    play->click(); // pause
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::shared_ptr<const render::LensFrame> lens;
    CHECK(waitFor([&] { return viewport->TakePendingFrame(width, height, rgba, lens); }));
    CHECK(width == 64 && height == 32 && !lens);
}

// Media ▸ Add clip to timeline… through its file dialog.
void testAddClipByMenu(const Ctx &c)
{
    answerFileDialog(c.clipB);
    trigger(c, "addClipAction");
    CHECK(c.timeline->count() == 2);
    if (c.timeline->count() == 2)
        CHECK(c.timeline->clipAt(1).sourcePath == c.clipB);
    CHECK(c.timeline->totalDurationMs() == 2 * kClipMs);
}

// Across the cut, the second clip's source is opened and playback goes on.
void testPlayAcrossCut(const Ctx &c)
{
    CHECK(seekTo(c, kClipMs - 200));
    auto *play = find<QToolButton>(c.w, "playerPlayButton");
    if (!play)
        return;
    play->click();
    CHECK(waitFor([&] { return c.timelineWidget->playhead() > kClipMs + 200; }));
    play->click();
    CHECK(wasOpened(c.clipB));
}

void testNavigationKeys(const Ctx &c)
{
    key(c, Qt::Key_Home);
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == 0; }));
    key(c, Qt::Key_End);
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == c.timeline->totalDurationMs(); }));
    key(c, Qt::Key_Up); // back to the cut
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == kClipMs; }));
    key(c, Qt::Key_Home);
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == 0; }));
    key(c, Qt::Key_Right); // a frame
    const qint64 frame = c.timeline->stepFrames(0, 1);
    CHECK(frame > 0);
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == frame; }));
    key(c, Qt::Key_Down); // on to the cut
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == kClipMs; }));
    key(c, Qt::Key_Right, Qt::ShiftModifier); // a second
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == kClipMs + 1000; }));
}

// Split, trim, restore, delete and speed on the video track, each undone.
// (Undo rebuilds the timeline, clip ids included: clips are looked up by
// their place.)
void testVideoEdits(const Ctx &c)
{
    QAction *undo = action(c, "undoAction");
    if (!undo)
        return;
    const auto clip = [&c](int index) -> const ClipItem * {
        return index < c.timeline->count() ? &c.timeline->clipAt(index) : nullptr;
    };

    settle(); // the clip added before is its own undo step
    CHECK(seekTo(c, 1000));
    key(c, Qt::Key_S);
    CHECK(c.timeline->count() == 3);
    CHECK(c.timeline->totalDurationMs() == 2 * kClipMs);
    undo->trigger();
    CHECK(c.timeline->count() == 2);

    settle();
    CHECK(seekTo(c, 1000));
    key(c, Qt::Key_BracketLeft); // trim in at the playhead
    CHECK(clip(0) && clip(0)->trimInMs == 1000);
    CHECK(c.timeline->totalDurationMs() == 2 * kClipMs - 1000);
    settle();
    CHECK(seekTo(c, 500));
    key(c, Qt::Key_R); // restore the clip under the playhead
    CHECK(clip(0) && clip(0)->trimInMs == 0 && clip(0)->trimOutMs == kClipMs);
    undo->trigger();
    CHECK(clip(0) && clip(0)->trimInMs == 1000);
    undo->trigger();
    CHECK(clip(0) && clip(0)->trimInMs == 0);
    CHECK(c.timeline->totalDurationMs() == 2 * kClipMs);

    // A clicked clip is the one Delete removes.
    settle();
    emit c.timelineWidget->clipSelected(c.timeline->clipAt(1).id);
    key(c, Qt::Key_Delete);
    CHECK(c.timeline->count() == 1 && clip(0)->sourcePath == c.clipA);
    undo->trigger();
    CHECK(c.timeline->count() == 2 && clip(1) && clip(1)->sourcePath == c.clipB);
    key(c, Qt::Key_Escape); // clears the selection

    settle();
    emit c.timelineWidget->clipSpeedRequested(c.timeline->clipAt(0).id, 2.0);
    CHECK(c.timeline->totalDurationMs() == kClipMs / 2 + kClipMs);
    undo->trigger();
    CHECK(c.timeline->totalDurationMs() == 2 * kClipMs);
}

// The Colour section edits the clip under the playhead; Copy / Paste grade.
void testColour(const Ctx &c)
{
    auto *exposure = find<QSlider>(c.w, "colourExposure");
    auto *copy = find<QPushButton>(c.w, "colourCopy");
    auto *paste = find<QPushButton>(c.w, "colourPaste");
    QAction *undo = action(c, "undoAction");
    if (!exposure || !copy || !paste || !undo)
        return;
    settle();
    CHECK(seekTo(c, 500));
    exposure->setValue(10); // +1.0 EV
    CHECK(std::abs(c.timeline->clipAt(0).color.exposureEv - 1.0f) < 1e-4f);
    copy->click();
    CHECK(paste->isEnabled());
    CHECK(seekTo(c, kClipMs + 500));
    CHECK(waitFor([&] { return exposure->value() == 0; })); // shows the second clip's grade
    settle();
    paste->click();
    CHECK(std::abs(c.timeline->clipAt(1).color.exposureEv - 1.0f) < 1e-4f);
    undo->trigger();
    CHECK(c.timeline->clipAt(1).color.exposureEv == 0.0f);
    undo->trigger();
    CHECK(c.timeline->clipAt(0).color.exposureEv == 0.0f);
}

// Files dropped on the Media tab are probed on worker threads; Add to
// timeline puts audio on a new audio track at the playhead.
void testMediaPool(const Ctx &c)
{
    auto *panel = find<MediaPoolPanel>(c.w, "mediaPoolPanel");
    if (!panel)
        return;
    emit panel->filesDropped({c.wav, c.mp4});
    MediaPool *pool = c.project->mediaPool();
    CHECK(waitFor([&] { return pool->contains(c.wav) && pool->contains(c.mp4); }));
    if (const MediaItem *m = pool->item(c.wav)) {
        CHECK(m->kind == MediaItem::Kind::Audio);
        CHECK(std::abs(m->durationMs - 3000) <= 50);
    }
    if (const MediaItem *m = pool->item(c.mp4)) {
        CHECK(m->kind == MediaItem::Kind::Video);
        CHECK(m->width == 320 && m->height == 180);
    }

    CHECK(seekTo(c, 1000));
    const int tracks = c.timeline->trackCount();
    settle();
    emit panel->addToTimelineRequested({c.wav});
    const int audioTrack = trackIdWhere(c, [&](const Track &t) {
        return t.group() == Track::Group::Audio && !t.clips.isEmpty();
    });
    CHECK(audioTrack != 0);
    if (const Track *t = track(c, audioTrack)) {
        CHECK(t->clips.size() == 1);
        CHECK(t->clips.first().sourcePath == c.wav && t->clips.first().startMs == 1000);
    }
    if (QAction *undo = action(c, "undoAction"))
        undo->trigger();
    CHECK(c.timeline->trackCount() == tracks);
}

// Add audio track, an audio file dropped on it, a split and a delete by key
// on the selected clip.
void testAudioTrack(const Ctx &c)
{
    QAction *undo = action(c, "undoAction");
    if (!undo)
        return;
    settle();
    const int tracks = c.timeline->trackCount();
    trigger(c, "addAudioTrackAction");
    CHECK(c.timeline->trackCount() == tracks + 1);
    const int id = trackIdWhere(c, [](const Track &t) {
        return t.group() == Track::Group::Audio && t.clips.isEmpty();
    });
    CHECK(id != 0);
    emit c.timelineWidget->audioFilesDropped({c.wav}, id, 500);
    const Track *t = track(c, id);
    CHECK(t && t->clips.size() == 1);
    if (!t || t->clips.size() != 1)
        return;
    const int clipId = t->clips.first().id;
    emit c.timelineWidget->trackClipSelected(clipId);
    CHECK(seekTo(c, 2000));
    settle();
    key(c, Qt::Key_S); // splits the selected audio clip, selects the right half
    t = track(c, id);
    CHECK(t && t->clips.size() == 2);
    settle();
    key(c, Qt::Key_Delete);
    t = track(c, id);
    CHECK(t && t->clips.size() == 1 && t->clips.first().id == clipId);
    if (t && !t->clips.isEmpty())
        CHECK(t->clips.first().trimOutMs - t->clips.first().trimInMs == 1500);
    undo->trigger(); // the delete
    undo->trigger(); // the split
    // Undo rebuilds the tracks with new ids.
    const int after = trackIdWhere(c, [&c](const Track &t) {
        return t.group() == Track::Group::Audio && !t.clips.isEmpty() &&
               t.clips.first().sourcePath == c.wav;
    });
    t = track(c, after);
    CHECK(t && t->clips.size() == 1);
    key(c, Qt::Key_Escape);
}

// Add overlay track, a video dropped on it: its pictures reach the viewport
// as an inset; K sets a key on the selected overlay.
void testOverlay(const Ctx &c)
{
    const int tracks = c.timeline->trackCount();
    trigger(c, "addOverlayTrackAction");
    CHECK(c.timeline->trackCount() == tracks + 1);
    const int id = trackIdWhere(c, [](const Track &t) {
        return t.kind == Track::Kind::Video && t.clips.isEmpty();
    });
    CHECK(id != 0);
    emit c.timelineWidget->videoFilesDropped({c.mp4}, id, 0);
    const Track *t = track(c, id);
    CHECK(t && t->clips.size() == 1);
    if (!t || t->clips.size() != 1)
        return;
    const int clipId = t->clips.first().id;
    CHECK(seekTo(c, 1000));
    CHECK(waitFor([&] {
        const auto insets = c.w->viewport()->Insets();
        return insets.size() == 1 && insets.front().clipId == clipId && insets.front().rgba;
    }));
    emit c.timelineWidget->trackClipSelected(clipId);
    key(c, Qt::Key_K);
    const TrackClip *clip = c.timeline->trackClipById(clipId);
    CHECK(clip && clip->insetKeys.count() == 1);
    key(c, Qt::Key_Escape);
}

// A second overlay track: the selected overlay's Bring forward puts its
// track above the other (the first overlay track draws on top).
void testOverlayLayers(const Ctx &c)
{
    const auto overlayTracks = [&c] {
        QList<int> ids;
        for (int i = 0; i < c.timeline->trackCount(); ++i)
            if (c.timeline->trackAt(i).kind == Track::Kind::Video)
                ids << c.timeline->trackAt(i).id;
        return ids;
    };
    settle();
    trigger(c, "addOverlayTrackAction");
    const QList<int> before = overlayTracks();
    CHECK(before.size() == 2);
    if (before.size() != 2)
        return;
    emit c.timelineWidget->videoFilesDropped({c.mp4}, before.last(), 0);
    const Track *t = track(c, before.last());
    CHECK(t && t->clips.size() == 1);
    if (!t || t->clips.isEmpty())
        return;
    emit c.timelineWidget->trackClipSelected(t->clips.first().id);
    auto *forward = find<QAbstractButton>(c.w, "overlayBringForward");
    auto *backward = find<QAbstractButton>(c.w, "overlaySendBackward");
    if (!forward || !backward)
        return;
    CHECK(forward->isEnabled() && !backward->isEnabled());
    forward->click();
    CHECK(overlayTracks() == (QList<int>{before.last(), before.first()}));
    CHECK(!forward->isEnabled() && backward->isEnabled());
    backward->click();
    CHECK(overlayTracks() == before);
    key(c, Qt::Key_Escape);
}

// F11 shows only the picture and the player bar; Esc leaves.
void testFullScreen(const Ctx &c)
{
    auto *timelinePane = find<QWidget>(c.w, "timelinePane");
    if (!timelinePane)
        return;
    key(c, Qt::Key_F11);
    CHECK(c.w->isFullScreen());
    CHECK(!timelinePane->isVisible());
    key(c, Qt::Key_Escape);
    CHECK(!c.w->isFullScreen());
    CHECK(timelinePane->isVisible());
}

// Track subject (T): the picture takes a box instead of a look-around drag;
// Esc cancels.
void testTrackSubjectMode(const Ctx &c)
{
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    CHECK(seekTo(c, 500));
    key(c, Qt::Key_T);
    CHECK(c.w->viewport()->BoxSelectMode());
    key(c, Qt::Key_Escape);
    CHECK(!c.w->viewport()->BoxSelectMode());
#else
    (void)c;
#endif
}

// The Media menu's entries that ask for files, and File ▸ Open project….
void testMenuFileDialogs(const Ctx &c)
{
    settle();
    CHECK(seekTo(c, 1500));
    const int tracks = c.timeline->trackCount();
    answerFileDialog(c.wav);
    trigger(c, "addAudioAction");
    CHECK(c.timeline->trackCount() == tracks + 1);
    const int audioTrack = trackIdWhere(c, [&c](const Track &t) {
        return t.group() == Track::Group::Audio && !t.clips.isEmpty() &&
               t.clips.first().startMs == 1500;
    });
    CHECK(audioTrack != 0);

    settle();
    answerFileDialog(c.mp4);
    trigger(c, "addVideoAction");
    CHECK(trackIdWhere(c, [&c](const Track &t) {
              return t.kind == Track::Kind::Video && !t.clips.isEmpty() &&
                     t.clips.first().startMs == 1500;
          }) != 0);

    const QString more = QDir(c.dir).filePath(QStringLiteral("more.wav"));
    testmedia::writeWav(more.toStdString(), testmedia::kWavRate, [](int) { return 0.0; });
    answerFileDialog(more);
    trigger(c, "importFilesAction");
    CHECK(waitFor([&] { return c.project->mediaPool()->contains(more); }));

    // Open project… asks about the unsaved changes first.
    CHECK(c.project->modified());
    answerMessageBox(QStringLiteral("Discard"));
    answerFileDialog(c.project->filePath());
    const int savedTracks = [&c] {
        Project saved;
        saved.load(c.project->filePath());
        return saved.timeline()->trackCount();
    }();
    trigger(c, "openProjectAction");
    CHECK(c.timeline->trackCount() == savedTracks);
    CHECK(!c.project->modified());
}

void testViewMenu(const Ctx &c)
{
    QSettings settings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"));
    trigger(c, "theme_light_Action");
    CHECK(settings.value(QStringLiteral("ui/theme")).toString() == QLatin1String("light"));
    trigger(c, "theme_ember_Action");
    CHECK(settings.value(QStringLiteral("ui/theme")).toString() == QLatin1String("ember"));

    // Preview size: a part of the project's stitch settings, undoable.
    QList<QAction *> sizes;
    for (QAction *a : c.w->findChildren<QAction *>())
        if (a->objectName().startsWith(QLatin1String("previewSize")) && a->isCheckable())
            sizes << a;
    CHECK(sizes.size() >= 2);
    for (QAction *a : sizes)
        if (!a->isChecked()) {
            const int before = c.project->previewStitch().width;
            settle();
            a->trigger();
            CHECK(a->isChecked());
            CHECK(c.project->previewStitch().width != before);
            if (QAction *undo = action(c, "undoAction"))
                undo->trigger();
            CHECK(c.project->previewStitch().width == before);
            break;
        }
}

void testHelpAndSettingsDialogs(const Ctx &c)
{
    for (const char *name : {"shortcutHelpAction", "aboutAction", "projectSettingsAction"}) {
        bool shown = false;
        whenDialog([&shown](QWidget *w) {
            shown = qobject_cast<QDialog *>(w) != nullptr;
            w->close();
        });
        trigger(c, name);
        CHECK(waitFor([&] { return shown; }));
        if (!shown)
            std::printf("      (%s)\n", name);
    }
}

// File ▸ Save project as… and File ▸ New project…: a project folder named
// in the dialog, the project saved into it.
void fillProjectDialog(const Ctx &c, const QString &name)
{
    whenDialog([&c, name](QWidget *w) {
        auto *nameEdit = w->findChild<QLineEdit *>(QStringLiteral("projectName"));
        auto *location = w->findChild<QLineEdit *>(QStringLiteral("projectLocation"));
        CHECK(nameEdit && location);
        if (nameEdit && location) {
            nameEdit->setText(name);
            location->setText(c.dir);
        }
        if (auto *dialog = qobject_cast<QDialog *>(w))
            dialog->accept();
    });
}

void testSaveAs(const Ctx &c)
{
    fillProjectDialog(c, QStringLiteral("Ride"));
    trigger(c, "saveProjectAsAction");
    const QString expected = QDir(c.dir).filePath(QStringLiteral("Ride/Ride.orp"));
    CHECK(c.project->filePath() == expected);
    CHECK(QFile::exists(expected));
    CHECK(!c.project->modified());
}

// Changes autosaved but never saved are offered when the project opens.
void testRecovery(const Ctx &c)
{
    const QString orp = c.project->filePath();
    const int tracks = c.timeline->trackCount();
    trigger(c, "addAudioTrackAction");
    CHECK(c.project->modified());
    auto *autosave = c.w->findChild<Autosave *>();
    CHECK(autosave);
    if (!autosave)
        return;
    autosave->tick();
    CHECK(Autosave::hasRecovery(orp));

    answerMessageBox(QStringLiteral("Recover"));
    c.w->openProject(orp);
    CHECK(c.timeline->trackCount() == tracks + 1);
    CHECK(c.project->modified()); // recovered, not saved yet
    trigger(c, "saveProjectAction");
    CHECK(!c.project->modified());
    CHECK(!Autosave::hasRecovery(orp));
}

// A clip's file moved: it shows as missing, and Locate file… relinks it.
void testLocateMovedFile(const Ctx &c)
{
    const QString moved = QDir(c.dir).filePath(QStringLiteral("moved.insv"));
    CHECK(QFile::rename(c.clipB, moved));
    c.w->openProject(c.project->filePath());
    CHECK(waitFor([&] { return c.timeline->count() == 2 && c.timeline->clipAt(1).mediaMissing; }));

    auto *panel = find<MediaPoolPanel>(c.w, "mediaPoolPanel");
    if (!panel)
        return;
    answerFileDialog(moved);
    emit panel->locateRequested(c.clipB);
    CHECK(c.timeline->clipAt(1).sourcePath == moved);
    CHECK(!c.timeline->clipAt(1).mediaMissing);
    CHECK(c.project->mediaPool()->contains(moved));
}

// Saved in place, then reopened in a second window.
void testSaveAndReopen(const Ctx &c)
{
    QString error;
    CHECK(c.project->save(c.orp, &error));
    const int tracks = c.timeline->trackCount();
    trigger(c, "addAudioTrackAction");
    CHECK(c.project->modified());
    trigger(c, "saveProjectAction");
    CHECK(!c.project->modified());

    MainWindow again(nullptr, standIns());
    auto *project2 = again.findChild<Project *>();
    again.openProject(c.orp);
    CHECK(project2 && project2->timeline()->count() == c.timeline->count());
    CHECK(project2 && project2->timeline()->trackCount() == tracks + 1);
    CHECK(project2 && !project2->modified());
}

// Leaving a modified project asks first: Cancel stays, Discard leaves.
void testCloseWithChanges(const Ctx &c)
{
    trigger(c, "addAudioTrackAction");
    CHECK(c.project->modified());
    auto *pages = c.w->findChild<QStackedWidget *>();
    QWidget *library = c.w->findChild<QWidget *>(QStringLiteral("projectLibrary"));

    answerMessageBox(QStringLiteral("Cancel"));
    trigger(c, "projectLibraryAction");
    CHECK(pages && pages->currentWidget() != library);

    answerMessageBox(QStringLiteral("Discard"));
    trigger(c, "projectLibraryAction");
    CHECK(pages && pages->currentWidget() == library);
}

void testNewProject(const Ctx &c)
{
    fillProjectDialog(c, QStringLiteral("Second"));
    trigger(c, "newProjectAction");
    const QString expected = QDir(c.dir).filePath(QStringLiteral("Second/Second.orp"));
    CHECK(c.project->filePath() == expected);
    CHECK(QFile::exists(expected));
    CHECK(c.timeline->count() == 0);
    auto *pages = c.w->findChild<QStackedWidget *>();
    CHECK(pages && pages->currentWidget() == find<QSplitter>(c.w, "editorSplitter"));
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // failures before a crash still show
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // Settings, the project library's list and folder, caches: not the user's.
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());
    qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/data")).toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (home.path() + QStringLiteral("/cache")).toLocal8Bit());
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("orbis-reframe"));
    theme::apply(app, theme::themes().first());

    Ctx c;
    c.dir = home.path();
    c.clipA = home.filePath(QStringLiteral("VID_20260101_120000_00_001.insv"));
    c.clipB = home.filePath(QStringLiteral("VID_20260101_121000_00_002.insv"));
    c.wav = home.filePath(QStringLiteral("tone.wav"));
    c.mp4 = home.filePath(QStringLiteral("phone.mp4"));
    c.orp = home.filePath(QStringLiteral("test.orp"));
    writeFile(c.clipA);
    writeFile(c.clipB);
    testmedia::writeWav(c.wav.toStdString(), 3 * testmedia::kWavRate, [](int i) {
        return 0.3 * std::sin(i * 2.0 * M_PI * 440.0 / testmedia::kWavRate);
    });
    testmedia::VideoSpec video;
    video.luma = [](int frame, int, int) { return uint8_t(40 + frame * 3); };
    CHECK(testmedia::writeVideo(c.mp4.toStdString(), video));

    {
        MainWindow w(nullptr, standIns());
        w.resize(1280, 760);
        w.showLibrary();
        w.show();
        w.activateWindow();
        CHECK(QTest::qWaitForWindowActive(&w));
        c.w = &w;
        c.project = w.findChild<Project *>();
        c.timelineWidget = w.findChild<TimelineWidget *>();
        CHECK(c.project && c.timelineWidget);
        if (c.project && c.timelineWidget) {
            c.timeline = c.project->timeline();
            testStartScreen(c);
            testOpenClip(c);
            testPlayback(c);
            testAddClipByMenu(c);
            testPlayAcrossCut(c);
            testNavigationKeys(c);
            testVideoEdits(c);
            testColour(c);
            testMediaPool(c);
            testAudioTrack(c);
            testOverlay(c);
            testOverlayLayers(c);
            testFullScreen(c);
            testTrackSubjectMode(c);
            testViewMenu(c);
            testHelpAndSettingsDialogs(c);
            testSaveAs(c);
            testMenuFileDialogs(c);
            testRecovery(c);
            testLocateMovedFile(c);
            testSaveAndReopen(c);
            testCloseWithChanges(c);
            testNewProject(c);
        }
    }

    if (g_failures == 0)
        std::printf("main_window_test: all checks passed\n");
    else
        std::printf("main_window_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
