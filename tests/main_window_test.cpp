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
#include "ClipMeasurements.h"
#include "MainWindow.h"
#include "SettingsPanel.h"
#include "SettingsSidebar.h"
#include "panels/StabilizationPanel.h"
#include "MediaPoolPanel.h"
#include "Theme.h"
#include "TimelineWidget.h"
#include "TabSidebar.h"
#include "TitlePresets.h"
#include "panels/MediaInfoPanel.h"
#include "support/TestMedia.h"
#include "support/TestZip.h"

#include "../core/Autosave.h"
#include "../core/DataPaths.h"
#include "../core/MediaPool.h"
#include "../core/Project.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMessageBox>
#include <QPlainTextEdit>
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
#include <map>
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
std::map<std::string, stitch::PreviewStitchConfig> g_openedWith; // the last open's config

bool wasOpened(const QString &path)
{
    std::lock_guard<std::mutex> lock(g_openedMutex);
    return g_opened.count(path.toStdString()) > 0;
}

bool lastOpenedWith(const QString &path, stitch::PreviewStitchConfig::LensGuard guard,
                    bool depthSeam)
{
    std::lock_guard<std::mutex> lock(g_openedMutex);
    const auto it = g_openedWith.find(path.toStdString());
    return it != g_openedWith.end() && it->second.lensGuard == guard &&
           it->second.depthSeam == depthSeam;
}

// Flat grey equirect frames, small, one per 1/30 s.
class StandInSource : public stitch::IStitchSource {
public:
    bool Open(const std::string &path, const stitch::PreviewStitchConfig &config) override
    {
        if (!QFile::exists(QString::fromStdString(path))) {
            m_error = "no such file";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_openedMutex);
            g_opened.insert(path);
            g_openedWith[path] = config;
        }
        m_info = {7680, 3840, kFps, kClipMs};
        // The second clip a lighter grey: which clip a picture came from.
        m_grey = QString::fromStdString(path).endsWith(QLatin1String("_002.insv")) ? 200 : 128;
        return true;
    }
    const stitch::ClipInfo &GetInfo() const override { return m_info; }
    stitch::Frame GetFrame(int64_t ms) override
    {
        SeekTo(ms);
        return ReadFrame();
    }
    // The first frame at or after `ms`, as the real sources seek (their
    // times rounded to the ms, as ReadFrame's).
    bool SeekTo(int64_t ms) override
    {
        m_frame = static_cast<int64_t>(std::floor(ms * kFps / 1000.0));
        while (frameMs(m_frame) < ms)
            ++m_frame;
        return true;
    }
    static int64_t frameMs(int64_t frame) { return std::llround(frame * 1000.0 / kFps); }
    stitch::Frame ReadFrame() override
    {
        stitch::Frame f;
        const int64_t ms = frameMs(m_frame);
        if (ms >= m_info.duration_ms)
            return f;
        f.width = 64;
        f.height = 32;
        f.rgba.assign(size_t(f.width) * f.height * 4, m_grey);
        f.timestamp_ms = ms;
        ++m_frame;
        ++g_framesServed;
        return f;
    }
    const std::string &GetLastError() const override { return m_error; }

private:
    stitch::ClipInfo m_info;
    int64_t m_frame = 0;
    uint8_t m_grey = 128;
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
        out = {7680, 3840, kFps, kClipMs, {}};
        // A ONE X2 take's lens-1 file and proxy are its VID_…_00_… clip
        // (camera::ClipFileOf does this by reading the clip).
        QString clip = QString::fromStdString(path);
        const QString name = QFileInfo(clip).fileName();
        if (name.startsWith(QLatin1String("VID_")) && name.contains(QLatin1String("_10_")))
            clip.replace(clip.lastIndexOf(QLatin1String("_10_")), 4, QStringLiteral("_00_"));
        else if (name.startsWith(QLatin1String("LRV_")) && name.contains(QLatin1String("_11_"))) {
            clip.replace(clip.lastIndexOf(QLatin1String("_11_")), 4, QStringLiteral("_00_"));
            clip.replace(clip.lastIndexOf(QLatin1String("LRV_")), 4, QStringLiteral("VID_"));
        }
        if (clip != QString::fromStdString(path))
            out.clip_path = clip.toStdString();
        // ... and needs its lens-1 file.
        const QString second = QString(clip).replace(QLatin1String("_00_"), QLatin1String("_10_"));
        if (second != clip && QFile::exists(second))
            out.other_files = {second.toStdString()};
        // A two-file take is a ONE X2's, which records no lens-guard
        // profiles; the one-file clips stand in for an X5's, which has both.
        out.lens_guards = out.other_files.empty() ? stitch::LensGuardProfiles::all()
                                                  : stitch::LensGuardProfiles{};
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

// For `ms`: any modal dialog that comes up is a failure (its text printed)
// and is closed, so it can't hang the test.
void failOnDialog(int ms)
{
    auto *timer = new QTimer;
    auto *elapsed = new QElapsedTimer;
    elapsed->start();
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, [timer, elapsed, ms] {
        if (QWidget *modal = QApplication::activeModalWidget()) {
            const auto *box = qobject_cast<QMessageBox *>(modal);
            std::printf("FAIL  unexpected dialog: %s\n",
                        box ? qPrintable(box->text()) : qPrintable(modal->windowTitle()));
            ++g_failures;
            modal->close();
        }
        if (elapsed->elapsed() < ms)
            return;
        timer->stop();
        timer->deleteLater();
        delete elapsed;
    });
    timer->start();
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
    QString clipA, clipB, wav, mp4, hlgMp4, orp;
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

// A key a trim left on clip 1's trim-out (clip 2's first frame) is reached
// with Shift+Down on clip 1's last frame, where K replaces it and Shift+K
// deletes it -- not clip 2's.
void testKeyOnTrimOut(const Ctx &c)
{
    if (c.timeline->count() < 2)
        return;
    const int a = c.timeline->clipAt(0).id;
    const qint64 out = c.timeline->clipAt(0).trimOutMs;
    c.timeline->upsertClipReframeKey(a, out, {});
    CHECK(seekTo(c, 500));
    key(c, Qt::Key_Down, Qt::ShiftModifier);
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == kClipMs - 1; }));
    settle();
    key(c, Qt::Key_K);
    CHECK(c.timeline->clipAt(0).reframe.keyTimes() == QList<qint64>{out});
    CHECK(c.timeline->clipAt(1).reframe.isEmpty());
    key(c, Qt::Key_K, Qt::ShiftModifier);
    CHECK(c.timeline->clipAt(0).reframe.isEmpty());
    settle();
}

// One frame step onto a cut shows the next clip's first frame, though it
// starts a hair after the cut (its trim-in between two frames) -- not the
// last clip's frame left on screen, which would get the next clip's view.
void testStepOntoCut(const Ctx &c)
{
    EquirectViewportWindow *viewport = c.w->viewport();
    if (c.timeline->count() < 2 || !viewport)
        return;
    const int b = c.timeline->clipAt(1).id;
    c.timeline->setClipTrimIn(b, 10); // its first frame: source 33 ms
    const qint64 cut = c.timeline->clipAt(0).timelineDurationMs();
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::shared_ptr<const render::LensFrame> lens;
    const auto shownGrey = [&](int grey) {
        return waitFor([&] {
            return viewport->TakePendingFrame(width, height, rgba, lens) && !rgba.empty() &&
                   rgba[0] == grey;
        });
    };
    CHECK(seekTo(c, cut));
    CHECK(shownGrey(200));
    key(c, Qt::Key_Left); // a frame back: the first clip's last
    CHECK(waitFor([&] { return c.timelineWidget->playhead() < cut; }));
    CHECK(shownGrey(128));
    key(c, Qt::Key_Right); // and on again
    CHECK(waitFor([&] { return c.timelineWidget->playhead() == cut; }));
    CHECK(shownGrey(200)); // the second clip's first
    settle();
    c.timeline->setClipTrimIn(b, 0);
    settle();
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
    // The edit toolbar's scissors do the same.
    settle();
    if (auto *split = find<QToolButton>(c.w, "splitButton")) {
        split->click();
        CHECK(c.timeline->count() == 3);
        undo->trigger();
        CHECK(c.timeline->count() == 2);
    }

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

    // The LUT the app ships for HLG isn't offered for a clip that isn't HLG.
    if (auto *lut = find<QComboBox>(c.w, "colourLut"))
        CHECK(lut->findText(QStringLiteral("HLG to Rec.709")) < 0);
}

// An HLG clip: the Colour section asks for a LUT and offers the one the app
// ships for HLG; picking it gives the clip a copy (or, without a project
// folder, the file itself), listed once.
void testHlgClip(const Ctx &c)
{
    auto *lut = find<QComboBox>(c.w, "colourLut");
    auto *wanted = find<QLabel>(c.w, "colourLutWanted");
    QAction *undo = action(c, "undoAction");
    const int before = c.timeline->count();
    emit c.timelineWidget->clipFilesDropped({c.hlgMp4}, before);
    CHECK(c.timeline->count() == before + 1);
    if (!lut || !wanted || !undo || c.timeline->count() != before + 1)
        return;
    // By position: undo rebuilds the clips with new ids.
    const auto clipLut = [&] {
        return c.timeline->count() > before
                   ? QFileInfo(c.timeline->clipAt(before).color.lut).fileName()
                   : QStringLiteral("(gone)");
    };
    CHECK(seekTo(c, c.timeline->clipToTimeline(before, 0) + 500));
    emit c.timelineWidget->clipSelected(c.timeline->clipAt(before).id);
    CHECK(waitFor([&] { return wanted->isVisibleTo(c.w); }));
    CHECK(wanted->text().contains(QStringLiteral("HLG")));
    // A packaged build has no source-tree fallback, so until it's installed
    // there's no shipped LUT to offer (package.sh checks the package has it).
    const bool shipped = QFileInfo::exists(QString::fromStdString(core::paths::lutsDir()) +
                                           QStringLiteral("/HLG to Rec.709.cube"));
    if (!shipped)
        std::printf("  (skipped the HLG LUT offer: no shipped LUTs in this build)\n");
    else
        CHECK(waitFor([&] { return lut->findText(QStringLiteral("HLG to Rec.709")) > 0; }));
    const int hlg = lut->findText(QStringLiteral("HLG to Rec.709"));
    if (hlg > 0) {
        settle();
        lut->setCurrentIndex(hlg);
        emit lut->activated(hlg);
        CHECK(clipLut() == QStringLiteral("HLG to Rec.709.cube"));
        CHECK(lut->findText(QStringLiteral("HLG to Rec.709")) == lut->currentIndex());
        undo->trigger();
        CHECK(clipLut().isEmpty());
    }
    if (c.timeline->count() > before)
        c.timeline->removeClip(c.timeline->clipAt(before).id);
    settle();
}

// Stitching is per clip's file: the panel edits the clip under the playhead,
// playback re-opens that file with it, Apply to all clips copies it.
void testStitching(const Ctx &c)
{
    using G = stitch::PreviewStitchConfig::LensGuard;
    auto *guard = find<QComboBox>(c.w, "stitchLensGuard");
    auto *depth = find<QCheckBox>(c.w, "stitchDepthSeam");
    auto *all = find<QPushButton>(c.w, "stitchApplyToAll");
    QAction *undo = action(c, "undoAction");
    if (!guard || !depth || !all || !undo)
        return;
    settle();
    CHECK(seekTo(c, 500));
    CHECK(guard->currentIndex() == int(G::Premium) && depth->isChecked());
    guard->setCurrentIndex(int(G::None));
    CHECK(c.timeline->clipAt(0).lensGuard == G::None);
    CHECK(c.timeline->clipAt(1).lensGuard == G::Premium); // another file
    CHECK(waitFor([&] { return lastOpenedWith(c.clipA, G::None, true); }));
    settle();
    CHECK(seekTo(c, kClipMs + 500));
    CHECK(waitFor([&] { return guard->currentIndex() == int(G::Premium); })); // the second clip's
    CHECK(waitFor([&] { return lastOpenedWith(c.clipB, G::Premium, true); }));
    depth->setChecked(false);
    CHECK(!c.timeline->clipAt(1).depthSeam && c.timeline->clipAt(0).lensGuard == G::None);
    CHECK(waitFor([&] { return lastOpenedWith(c.clipB, G::Premium, false); }));
    settle();
    all->click();
    CHECK(c.timeline->clipAt(0).lensGuard == G::Premium && !c.timeline->clipAt(0).depthSeam);
    undo->trigger();
    CHECK(c.timeline->clipAt(0).lensGuard == G::None && c.timeline->clipAt(0).depthSeam);
    undo->trigger();
    CHECK(c.timeline->clipAt(1).depthSeam);
    undo->trigger();
    CHECK(c.timeline->clipAt(0).lensGuard == G::Premium);
    CHECK(waitFor([&] { return guard->currentIndex() == int(G::Premium) && depth->isChecked(); }));
}

// A clip whose file records no lens-guard profiles (a ONE X2 take) comes in
// with None, and the panel shows it greyed out with a line saying why.
// Apply to all clips copies only the depth-aware seam from it, and leaves
// its guard alone when applied from a clip that has profiles.
void testStitchingWithoutGuards(const Ctx &c)
{
    using G = stitch::PreviewStitchConfig::LensGuard;
    auto *guard = find<QComboBox>(c.w, "stitchLensGuard");
    auto *depth = find<QCheckBox>(c.w, "stitchDepthSeam");
    auto *note = find<QLabel>(c.w, "stitchNoGuards");
    auto *all = find<QPushButton>(c.w, "stitchApplyToAll");
    QAction *undo = action(c, "undoAction");
    if (!guard || !depth || !note || !all || !undo)
        return;
    settle();
    const QString dir = QDir(c.dir).filePath(QStringLiteral("x2guards"));
    QDir().mkpath(dir);
    const QString lead = dir + QStringLiteral("/VID_20250816_110155_00_004.insv");
    writeFile(lead);
    writeFile(dir + QStringLiteral("/VID_20250816_110155_10_004.insv"));
    const int n = c.timeline->count();
    CHECK(c.timeline->clipAt(n - 1).lensGuard == G::Premium); // what it would inherit
    emit c.timelineWidget->clipFilesDropped({lead}, n);
    CHECK(waitFor([&] { return c.timeline->count() == n + 1; }));
    if (c.timeline->count() != n + 1)
        return;
    CHECK(c.timeline->clipAt(n).lensGuard == G::None);

    // Its panel: None, greyed out, the line shown; an X5 clip's isn't.
    qint64 start = 0;
    for (int i = 0; i < n; ++i)
        start += c.timeline->clipAt(i).timelineDurationMs();
    settle();
    CHECK(seekTo(c, start + 500));
    CHECK(waitFor([&] { return guard->currentIndex() == int(G::None) && !guard->isEnabled(); }));
    CHECK(!note->isHidden());
    CHECK(seekTo(c, 500));
    CHECK(waitFor([&] { return guard->currentIndex() == int(G::Premium) && guard->isEnabled(); }));
    CHECK(note->isHidden());

    // Apply to all from the X5 clip: the take keeps None.
    all->click();
    CHECK(c.timeline->clipAt(n).lensGuard == G::None);
    // ... and from the take: only its depth-aware seam goes onto the X5 clips.
    settle();
    CHECK(seekTo(c, start + 500));
    CHECK(waitFor([&] { return !guard->isEnabled(); }));
    depth->setChecked(false);
    settle();
    all->click();
    CHECK(c.timeline->clipAt(0).lensGuard == G::Premium && !c.timeline->clipAt(0).depthSeam);
    undo->trigger();
    undo->trigger();
    undo->trigger();
    CHECK(c.timeline->count() == n);
    CHECK(c.timeline->clipAt(0).lensGuard == G::Premium && c.timeline->clipAt(0).depthSeam);
}

// Direction lock: picking it holds the direction under the playhead, the
// button takes it again where the playhead is now, and pasting it onto a
// clip of another file holds that clip's first frame.
void testDirectionLock(const Ctx &c)
{
    auto *mode = find<QComboBox>(c.w, "stabilizationMode");
    auto *lock = find<QPushButton>(c.w, "stabilizationLockDirection");
    auto *where = find<QLabel>(c.w, "stabilizationDirectionStatus");
    QAction *undo = action(c, "undoAction");
    if (!mode || !lock || !where || !undo)
        return;
    const int a = c.timeline->clipAt(0).id;
    const int b = c.timeline->clipAt(1).id;
    const insv::StabConfig beforeA = c.timeline->clipAt(0).stabilization;
    const insv::StabConfig beforeB = c.timeline->clipAt(1).stabilization;
    settle();
    CHECK(seekTo(c, 1500));
    settle();
    mode->setCurrentIndex(mode->findData(int(StabilizationPanel::Mode::DirectionLock)));
    const insv::StabConfig *s = &c.timeline->clipById(a)->stabilization;
    CHECK(s->mode == insv::StabMode::DirectionLock && s->lockDirection);
    CHECK(std::fabs(s->directionSec - 1.5f) < 0.01f);
    CHECK(waitFor([&] { return where->text().contains(QLatin1String("00:00:01")); }));

    settle();
    CHECK(seekTo(c, 3000));
    settle();
    lock->click();
    s = &c.timeline->clipById(a)->stabilization;
    CHECK(std::fabs(s->directionSec - 3.0f) < 0.01f);

    settle();
    emit c.timelineWidget->stabilizationCopyRequested(a);
    emit c.timelineWidget->stabilizationPasteRequested(b);
    const ClipItem *cb = c.timeline->clipById(b);
    CHECK(cb && cb->stabilization.lockDirection);
    if (cb)
        CHECK(std::fabs(cb->stabilization.directionSec - cb->trimInMs / 1000.0f) < 0.01f);

    for (int i = 0; i < 3; ++i)
        undo->trigger();
    CHECK(c.timeline->clipAt(0).stabilization == beforeA);
    CHECK(c.timeline->clipAt(1).stabilization == beforeB);
}

// Split screen from its section: a grid gives the new panes Fill panes'
// views; the pane picked in the Reframe section (or by a click in the view)
// is what keys go to; Fill panes keys every pane; undo takes it all back.
void testSplitScreen(const Ctx &c)
{
    auto *layout = find<QComboBox>(c.w, "splitLayoutCombo");
    auto *paneCombo = find<QComboBox>(c.w, "activePaneCombo");
    auto *gap = find<QDoubleSpinBox>(c.w, "splitGapBox");
    auto *fill = find<QPushButton>(c.w, "fillPanesButton");
    QAction *undo = action(c, "undoAction");
    if (!layout || !paneCombo || !gap || !fill || !undo)
        return;
    EquirectViewportWindow *v = c.w->viewport();
    const int a = c.timeline->clipAt(0).id;
    const ClipItem before = c.timeline->clipAt(0);
    settle();
    CHECK(seekTo(c, 1000));
    settle();
    CHECK(!paneCombo->isVisibleTo(c.w) && !gap->isVisibleTo(c.w));
    v->SetPose({0.5f, 0.0f, 0.0f, 1.2f}); // pane 1 looks this way
    const int grid = layout->findData(int(pane::Layout::Grid));
    layout->setCurrentIndex(grid);
    emit layout->activated(grid);
    const ClipItem *clip = c.timeline->clipById(a);
    CHECK(clip->split.layout == pane::Layout::Grid && clip->paneCount() == 4);
    CHECK(clip->reframe.keyTimes() == before.reframe.keyTimes()); // pane 1 untouched
    const qint64 at = clip->sourceAtLocalMs(1000);
    for (int i = 1; i < 4; ++i) {
        const ReframeTrack &t = clip->paneTrack(i);
        CHECK(t.count() == 1);
        if (t.count() == 1)
            CHECK(t.keys()[0].timeMs == at &&
                  std::fabs(t.keys()[0].pose.yaw -
                            (0.5f + pane::fillYawOffset(pane::Layout::Grid, i))) < 1e-4f);
    }
    // The viewport draws the grid; its own pose is the active pane's.
    CHECK(v->Split().layout == pane::Layout::Grid);
    CHECK(paneCombo->isVisibleTo(c.w) && paneCombo->count() == 4 && gap->isVisibleTo(c.w));
    CHECK(std::fabs(v->ViewAspect() - 16.0f / 9.0f) < 0.05f); // a quarter of 16:9 is 16:9

    // Pane 3 (bottom left) picked: the view shows it, keys go to it.
    settle();
    emit paneCombo->activated(2);
    CHECK(v->ActivePane() == 2);
    CHECK(std::fabs(v->Yaw() - (0.5f + pane::fillYawOffset(pane::Layout::Grid, 2))) < 1e-4f);
    CHECK(seekTo(c, 2000));
    key(c, Qt::Key_K);
    clip = c.timeline->clipById(a);
    CHECK(clip->paneTrack(2).count() == 2 && clip->reframe.keyTimes() == before.reframe.keyTimes());
    key(c, Qt::Key_K, Qt::ShiftModifier); // and come off it
    CHECK(c.timeline->clipById(a)->paneTrack(2).count() == 1);

    // Fill panes: a key in every pane at the playhead.
    settle();
    fill->click();
    clip = c.timeline->clipById(a);
    for (int i = 0; i < 4; ++i)
        CHECK(clip->paneTrack(i).keyTimes().contains(clip->sourceAtLocalMs(2000)));

    // The divider.
    settle();
    gap->setValue(1.0);
    CHECK(std::fabs(c.timeline->clipById(a)->split.gap - 0.01f) < 1e-6f);

    // Side by side: the panes are named by where they are; pane 3 isn't
    // there, so the view steers pane 1.
    settle();
    const int side = layout->findData(int(pane::Layout::SideBySide));
    layout->setCurrentIndex(side);
    emit layout->activated(side);
    CHECK(c.timeline->clipById(a)->paneCount() == 2);
    CHECK(paneCombo->count() == 2 && paneCombo->itemText(1) == QLatin1String("Right"));
    CHECK(v->EffectiveActivePane() == 0);

    // Undo back to before the split (undo rebuilds the clips: new ids).
    settle();
    const auto unsplit = [&] {
        const ClipItem &first = c.timeline->clipAt(0);
        return first.split == before.split && first.panes[0].isEmpty() &&
               first.panes[1].isEmpty() && first.panes[2].isEmpty();
    };
    for (int i = 0; i < 8 && !unsplit(); ++i)
        undo->trigger();
    CHECK(unsplit());
    CHECK(c.timeline->clipAt(0).reframe.keyTimes() == before.reframe.keyTimes());
    c.w->viewport()->SetActivePane(0);
    emit paneCombo->activated(0);
    settle();
}

// Duplicate (the clip menu): the copy lands after the clip and is selected,
// so the panels edit it; one undo step takes it away.
void testDuplicateClip(const Ctx &c)
{
    auto *exposure = find<QSlider>(c.w, "colourExposure");
    QAction *undo = action(c, "undoAction");
    if (!exposure || !undo)
        return;
    settle();
    const int before = c.timeline->count();
    const int a = c.timeline->clipAt(0).id;
    emit c.timelineWidget->clipDuplicateRequested(a);
    CHECK(c.timeline->count() == before + 1);
    if (c.timeline->count() != before + 1)
        return;
    const int copy = c.timeline->clipAt(1).id;
    CHECK(copy != a && c.timeline->clipAt(1).sourcePath == c.timeline->clipAt(0).sourcePath);
    CHECK(c.timeline->totalDurationMs() == (before + 1) * kClipMs);
    settle();
    exposure->setValue(-10); // the selected clip: the copy
    CHECK(std::abs(c.timeline->clipById(copy)->color.exposureEv + 1.0f) < 1e-4f);
    CHECK(c.timeline->clipById(a)->color.exposureEv == 0.0f);
    undo->trigger();
    undo->trigger();
    CHECK(c.timeline->count() == before);
    emit c.timelineWidget->clipSelected(0); // nothing selected again
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

// A ONE X2 take is three files: lens 0 (the clip), lens 1, the proxy. A
// folder of them imports as one clip, the take's; all three dropped on the
// timeline add it once. No error for the other two.
void testTakeFiles(const Ctx &c)
{
    auto *panel = find<MediaPoolPanel>(c.w, "mediaPoolPanel");
    if (!panel)
        return;
    const QString dir = QDir(c.dir).filePath(QStringLiteral("x2card"));
    QDir().mkpath(dir);
    const QString lead = dir + QStringLiteral("/VID_20250816_103807_00_001.insv");
    const QString second = dir + QStringLiteral("/VID_20250816_103807_10_001.insv");
    const QString proxy = dir + QStringLiteral("/LRV_20250816_103807_11_001.insv");
    for (const QString &f : {lead, second, proxy})
        writeFile(f);
    MediaPool *pool = c.project->mediaPool();
    const int items = pool->count();
    failOnDialog(1500);
    emit panel->filesDropped({dir});
    CHECK(waitFor([&] { return pool->contains(lead); }));
    QTest::qWait(1500); // every probe in, and no dialog
    CHECK(pool->count() == items + 1);
    CHECK(!pool->contains(second) && !pool->contains(proxy));
    if (const MediaItem *m = pool->item(lead))
        CHECK(m->companions == QStringList{second});
    pool->remove({lead});

    const int clips = c.timeline->count();
    failOnDialog(500);
    emit c.timelineWidget->clipFilesDropped({second, lead, proxy}, clips);
    QTest::qWait(500);
    CHECK(c.timeline->count() == clips + 1);
    if (c.timeline->count() == clips + 1)
        CHECK(c.timeline->clipAt(clips).sourcePath == lead);
    // In the pool as probed, the lens-1 file with it.
    if (const MediaItem *m = pool->item(lead))
        CHECK(m->companions == QStringList{second});
    if (QAction *undo = action(c, "undoAction"))
        undo->trigger();
    CHECK(c.timeline->count() == clips);
    pool->remove({lead});
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
    // Undo rebuilds the tracks, with the ids they had.
    t = track(c, id);
    CHECK(t && t->clips.size() == 1 && t->clips.first().sourcePath == c.wav);
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
    auto *keyCount = find<QLabel>(c.w, "overlayKeyCount");
    CHECK(keyCount && keyCount->text() == QStringLiteral("1 key"));
    CHECK(seekTo(c, 1500)); // the overlay is 2 s long
    key(c, Qt::Key_K);
    clip = c.timeline->trackClipById(clipId);
    CHECK(clip && clip->insetKeys.count() == 2);
    CHECK(keyCount && keyCount->text() == QStringLiteral("2 keys"));
    key(c, Qt::Key_Escape);
}

// A flat video dropped on the main track: a main clip of its own, drawn as
// a full-frame layer; the 360-only panels say they don't apply, and the
// Reframe section offers Fit instead.
void testFlatMainClip(const Ctx &c)
{
    const int before = c.timeline->count();
    emit c.timelineWidget->clipFilesDropped({c.mp4}, before);
    CHECK(c.timeline->count() == before + 1);
    if (c.timeline->count() != before + 1)
        return;
    const ClipItem &flat = c.timeline->clipAt(before);
    const int id = flat.id;
    CHECK(flat.flat && flat.sourceWidth == 320 && flat.sourceHeight == 180);
    CHECK(c.project->mediaPool()->item(c.mp4) &&
          c.project->mediaPool()->item(c.mp4)->kind == MediaItem::Kind::Video);
    const qint64 start = c.timeline->clipToTimeline(before, 0);
    CHECK(seekTo(c, start + 500));
    CHECK(waitFor([&] {
        for (const auto &d : c.w->viewport()->Insets())
            if (d.clipId == id && d.rgba && d.look.width == 1.0f)
                return true;
        return false;
    }));
    emit c.timelineWidget->clipSelected(id);
    auto *stab = find<SettingsPanel>(c.w, "stabilizationPanel");
    auto *privacy = find<SettingsPanel>(c.w, "privacyPanel");
    auto *stitching = find<SettingsPanel>(c.w, "stitchingPanel");
    CHECK(stab && !stab->isApplicable() && privacy && !privacy->isApplicable());
    CHECK(stitching && !stitching->isApplicable());
    auto *fit = find<QComboBox>(c.w, "reframeFitCombo");
    CHECK(fit && fit->isVisibleTo(c.w) && fit->currentIndex() == 0);
    if (fit)
        emit fit->activated(1);
    CHECK(c.timeline->clipById(id)->fit == ClipItem::Fit::Fill);
    // Locked in place: the viewport doesn't let it be picked up and dragged.
    for (const auto &d : c.w->viewport()->Insets())
        if (d.clipId == id)
            CHECK(d.locked);
    // Its colour grade reaches the picture, paused (the frame doesn't change).
    ColorGrade grey;
    grey.saturation = -100.0f;
    c.timeline->setClipColor(id, grey);
    CHECK(waitFor([&] {
        for (const auto &d : c.w->viewport()->Insets())
            if (d.clipId == id && d.colorGrade.adjust == grey.packed())
                return true;
        return false;
    }));
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    key(c, Qt::Key_T); // nothing to track on a flat clip
    CHECK(!c.w->viewport()->BoxSelectMode());
#endif
    // Back on a 360 clip, everything applies again.
    emit c.timelineWidget->clipSelected(c.timeline->clipAt(0).id);
    CHECK(stab && stab->isApplicable() && fit && !fit->isVisibleTo(c.w));
    CHECK(stitching && stitching->isApplicable());
    c.timeline->removeClip(id);
    settle();
}

// The Transitions tab's "Add at the nearest cut": a crossfade at the cut
// nearest the playhead, selected, the Transition tab showing it -- why it
// doesn't play with no footage past the trims, its duration edited there;
// Delete removes it, undo puts it back, a click on a clip deselects it.
void testTransition(const Ctx &c)
{
    QAction *undo = action(c, "undoAction");
    CHECK(find<QWidget>(c.w, "transitionsPanel") && c.timeline->count() == 2);
    if (!undo || c.timeline->count() != 2)
        return;
    settle();
    CHECK(seekTo(c, kClipMs - 300));
    trigger(c, "transitionAddAction");
    const int left = c.timeline->clipAt(0).id;
    const Transition *t = c.timeline->transitionAfter(left);
    CHECK(t && t->durationMs == Transition::kDefaultMs && c.timeline->transitions().size() == 1);
    auto *sidebar = c.w->findChild<SettingsSidebar *>();
    CHECK(sidebar && sidebar->currentIndex() == SettingsSidebar::TransitionTab);
    auto *duration = find<QDoubleSpinBox>(c.w, "transitionDuration");
    auto *note = find<QLabel>(c.w, "transitionNote");
    CHECK(duration && duration->isEnabled() && std::abs(duration->value() - 1.0) < 1e-6);
    // Untrimmed clips: no footage past the cut, so it doesn't play.
    CHECK(note && note->isVisibleTo(c.w) && note->text().startsWith(QStringLiteral("Doesn't play")));
    // With room past the trims, it plays in full.
    settle();
    c.timeline->setClipTrimOut(left, kClipMs - 1000);
    c.timeline->setClipTrimIn(c.timeline->clipAt(1).id, 1000);
    CHECK(note && !note->isVisibleTo(c.w));
    settle();
    if (duration)
        duration->setValue(1.5);
    t = c.timeline->transitionAfter(left);
    CHECK(t && t->durationMs == 1500);
    CHECK(c.timeline->transitionSpan(*t).durationMs == 1500);
    // A dip to black put on the same cut replaces it, keeping its length.
    emit c.timelineWidget->transitionDropped(left, QStringLiteral("dipToBlack"));
    t = c.timeline->transitionAfter(left);
    CHECK(t && t->type == Transition::Type::DipToBlack && t->durationMs == 1500);
    CHECK(c.timeline->transitions().size() == 1);

    settle();
    key(c, Qt::Key_Delete); // the selected transition, not a clip or key
    CHECK(c.timeline->transitions().isEmpty() && c.timeline->count() == 2);
    CHECK(duration && !duration->isEnabled());
    undo->trigger();
    CHECK(c.timeline->transitions().size() == 1);
    // Clicking a clip selects it instead.
    const int leftNow = c.timeline->clipAt(0).id; // undo gave the clips new ids
    emit c.timelineWidget->transitionSelected(leftNow);
    CHECK(duration && duration->isEnabled());
    emit c.timelineWidget->clipSelected(c.timeline->clipAt(1).id);
    CHECK(duration && !duration->isEnabled());
    key(c, Qt::Key_Escape);

    // Back as it was: no transition, the clips untrimmed.
    settle();
    c.timeline->removeTransition(leftNow);
    c.timeline->clearClipTrim(c.timeline->clipAt(0).id);
    c.timeline->clearClipTrim(c.timeline->clipAt(1).id);
    settle();
}

// The Titles tab's "Add to timeline": a title at the playhead, selected, the
// Title tab showing its text; typing there edits it (the K shortcut
// doesn't set a key), the picture reaches the viewport, and Duration sets
// its length.
void testTitle(const Ctx &c)
{
    CHECK(find<QWidget>(c.w, "titlesPanel"));
    CHECK(seekTo(c, 500));
    trigger(c, "titleAddToTimelineAction");
    int clipId = 0;
    for (int i = 0; i < c.timeline->trackCount(); ++i)
        for (const TrackClip &tc : c.timeline->trackAt(i).clips)
            if (tc.isTitle())
                clipId = tc.id;
    const TrackClip *clip = c.timeline->trackClipById(clipId);
    CHECK(clip && clip->startMs == 500 && clip->title->text == QStringLiteral("Title"));
    if (!clip)
        return;
    // The settings sidebar is on the Title tab.
    auto *sidebar = c.w->findChild<SettingsSidebar *>();
    CHECK(sidebar && sidebar->currentIndex() == SettingsSidebar::TitleTab);
    auto *text = find<QPlainTextEdit>(c.w, "titleText");
    CHECK(text && text->isVisibleTo(c.w) && text->toPlainText() == QStringLiteral("Title"));
    CHECK(waitFor([&] {
        for (const auto &d : c.w->viewport()->Insets())
            if (d.clipId == clipId && d.rgba && d.width > d.height)
                return true;
        return false;
    }));
    if (text) {
        text->setFocus();
        text->moveCursor(QTextCursor::End);
        key(c, Qt::Key_K);
        clip = c.timeline->trackClipById(clipId);
        CHECK(clip && clip->title->text == QStringLiteral("Titlek") && clip->insetKeys.isEmpty());
        text->clearFocus();
    }
    // Its duration, from the tab: the start stays, the end moves.
    auto *duration = find<QDoubleSpinBox>(c.w, "titleDuration");
    CHECK(duration && std::abs(duration->value() - 5.0) < 1e-6);
    if (duration)
        duration->setValue(8.0);
    clip = c.timeline->trackClipById(clipId);
    CHECK(clip && clip->startMs == 500 && clip->timelineDurationMs() == 8000);
    // Left-aligned from the Text section: the text stays where it was (its
    // position becomes the text's left edge), and one undo puts both back.
    auto *left = find<QPushButton>(c.w, "titleAlignLeft");
    QAction *undo = action(c, "undoAction");
    if (clip && left && undo) {
        settle();
        const float x0 = clip->look.centerX;
        left->click();
        clip = c.timeline->trackClipById(clipId);
        CHECK(clip && clip->title->align == title::Align::Left && clip->look.centerX < x0 - 0.01f);
        undo->trigger();
        clip = nullptr; // undo rebuilds the timeline, ids included
        for (int i = 0; i < c.timeline->trackCount(); ++i)
            for (const TrackClip &tc : c.timeline->trackAt(i).clips)
                if (tc.isTitle())
                    clip = &tc;
        CHECK(clip && clip->title->align == title::Align::Centre && clip->look.centerX == x0);
        if (clip)
            clipId = clip->id;
    }
    // An animation from the Animation section: on the clip, one undo step.
    auto *motionIn = find<QComboBox>(c.w, "titleMotionIn");
    if (clip && motionIn && undo) {
        emit c.w->viewport()->insetPressed(clipId); // the undo let go of the selection
        settle();
        motionIn->setCurrentIndex(1); // slide in from the left
        clip = c.timeline->trackClipById(clipId);
        CHECK(clip && clip->motion.in.kind == inset::Motion::Kind::Slide &&
              clip->motion.in.side == inset::Motion::Side::Left);
        undo->trigger();
        clip = nullptr;
        for (int i = 0; i < c.timeline->trackCount(); ++i)
            for (const TrackClip &tc : c.timeline->trackAt(i).clips)
                if (tc.isTitle())
                    clip = &tc;
        CHECK(clip && clip->motion.isNone());
        if (clip)
            clipId = clip->id;
    }
    // The Overlay tab shows overlay videos only.
    auto *overlayX = find<QDoubleSpinBox>(c.w, "overlayDuration");
    CHECK(overlayX && !overlayX->isEnabled());
    c.timeline->removeTrackClip(clipId);
    settle();
}

// The Titles tab's ready-made titles: a tile per preset, dragged out as its
// id; adding one, by the tab or by a drop, gives an ordinary title with the
// preset's style and place. A date tile writes the clip's recording date,
// where its file says.
void testTitlePresets(const Ctx &c)
{
    const QList<titlepresets::Preset> &presets = titlepresets::all();
    auto *list = find<QListWidget>(c.w, "titleList");
    CHECK(list && list->count() == presets.size());
    if (!list || list->count() != presets.size())
        return;
    const auto titles = [&c] {
        QList<const TrackClip *> out;
        for (int i = 0; i < c.timeline->trackCount(); ++i)
            for (const TrackClip &tc : c.timeline->trackAt(i).clips)
                if (tc.isTitle())
                    out << &tc;
        return out;
    };
    const auto added = [&](const titlepresets::Preset &p, qint64 atMs) {
        const QList<const TrackClip *> all = titles();
        const TrackClip *clip = all.size() == 1 ? all.front() : nullptr;
        CHECK(clip && clip->startMs == atMs && clip->look == p.look.clamped());
        CHECK(clip && clip->title->align == p.style.align && clip->title->box == p.style.box &&
              clip->title->shadow == p.style.shadow && clip->title->outline == p.style.outline);
        return clip;
    };
    for (int row = 0; row < list->count(); ++row) {
        const titlepresets::Preset &p = presets[row];
        CHECK(p.style == p.style.clamped() && p.look == p.look.clamped());
        std::unique_ptr<QMimeData> mime(list->model()->mimeData({list->model()->index(row, 0)}));
        CHECK(mime && mime->data(QString::fromLatin1(title::kMimeType)) == p.id.toLatin1());
    }

    // An animated tile plays while the mouse is over it, and is back at
    // rest when it leaves; a still one stays as it is.
    {
        const auto tileImage = [list](int row) {
            return list->item(row)->icon().pixmap(list->iconSize()).toImage();
        };
        // Only a tile on screen plays: the Titles tab showing.
        TabSidebar *sources = nullptr;
        for (QObject *o = list; o && !sources; o = o->parent())
            sources = qobject_cast<TabSidebar *>(o);
        const int tabBefore = sources ? sources->currentIndex() : 0;
        const bool openBefore = sources && sources->isOpen();
        if (sources) {
            sources->setOpen(true);
            sources->setCurrentIndex(1); // Media, Titles, Transitions
        }
        settle();
        CHECK(list->isVisible());
        const int sliding = int(titlepresets::find(QStringLiteral("lower-third-slide")) -
                                presets.data());
        const QImage rest = tileImage(sliding);
        const auto pointAt = [list](int row) {
            QCursor::setPos(list->viewport()->mapToGlobal(
                list->visualItemRect(list->item(row)).center()));
        };
        list->scrollToItem(list->item(sliding));
        pointAt(sliding);
        emit list->itemEntered(list->item(sliding));
        QTest::qWait(100); // sliding in
        const QImage early = tileImage(sliding);
        // A Leave while still on it changes nothing (it keeps sliding); off
        // it, the tile rests.
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(list->viewport(), &leave);
        QTest::qWait(150);
        CHECK(early != rest && tileImage(sliding) != early && tileImage(sliding) != rest);
        QCursor::setPos(c.w->mapToGlobal(QPoint(c.w->width() / 2, c.w->height() / 2)));
        QTest::qWait(100);
        CHECK(tileImage(sliding) == rest);
        const QImage plain = tileImage(0);
        list->scrollToItem(list->item(0));
        pointAt(0);
        emit list->itemEntered(list->item(0));
        QTest::qWait(150);
        CHECK(tileImage(0) == plain);
        QCoreApplication::sendEvent(list->viewport(), &leave);
        if (sources) {
            sources->setCurrentIndex(tabBefore);
            sources->setOpen(openBefore);
        }
    }

    // From the tab, at the playhead.
    CHECK(seekTo(c, 500));
    const titlepresets::Preset *lower = titlepresets::find(QStringLiteral("lower-third"));
    CHECK(lower);
    if (!lower)
        return;
    list->setCurrentRow(int(lower - presets.data()));
    trigger(c, "titleAddToTimelineAction");
    const TrackClip *clip = added(*lower, 500);
    CHECK(clip && clip->title->text == lower->style.text);
    if (clip)
        c.timeline->removeTrackClip(clip->id);

    // Dropped on the timeline; a date tile's date is the main clip's.
    const titlepresets::Preset *date = titlepresets::find(QStringLiteral("date-place"));
    CHECK(date);
    if (!date)
        return;
    emit c.timelineWidget->titleDropped(0, 700, date->id);
    clip = added(*date, 700);
    camera::MediaInfo info;
    std::string err;
    const QDateTime recorded =
        camera::ProbeMediaInfo(c.timeline->clipAt(0).sourcePath.toStdString(), info, err)
            ? recordedTime(info)
            : QDateTime();
    CHECK(clip && clip->title->text == titlepresets::styleFor(*date, recorded).text);
    CHECK(clip && clip->title->text.endsWith(QStringLiteral("\nPlace")));
    if (clip)
        c.timeline->removeTrackClip(clip->id);

    // An id this build doesn't know: the plain title.
    emit c.timelineWidget->titleDropped(0, 700, QStringLiteral("no-such-tile"));
    clip = added(presets.front(), 700);
    CHECK(clip && clip->title->text == QStringLiteral("Title"));
    if (clip)
        c.timeline->removeTrackClip(clip->id);
    settle();
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
    // A folded overlay track stays folded through an undo, which rebuilds
    // the tracks.
    c.timelineWidget->setSoundCollapsed(before.first(), true);
    settle();
    backward->click();
    CHECK(overlayTracks() == before);
    QAction *undo = action(c, "undoAction");
    if (undo)
        undo->trigger();
    // (Asked by the tracks there now: a rebuild with new ids left the old
    // ones folded, matching no track.)
    CHECK(overlayTracks() == (QList<int>{before.last(), before.first()}));
    CHECK(c.timelineWidget->isSoundCollapsed(overlayTracks().last()));
    if (QAction *redo = action(c, "redoAction"))
        redo->trigger();
    CHECK(overlayTracks() == before);
    CHECK(c.timelineWidget->isSoundCollapsed(overlayTracks().first()));
    CHECK(!c.timelineWidget->isSoundCollapsed(overlayTracks().last()));
    c.timelineWidget->setSoundCollapsed(before.first(), false);
    key(c, Qt::Key_Escape);
}

// F11 shows only the picture and the player bar; Esc leaves.
void testFullScreen(const Ctx &c)
{
    auto *timelinePane = find<QWidget>(c.w, "timelinePane");
    if (!timelinePane)
        return;
    // The app's state, not isFullScreen(): Qt 6.12's offscreen platform
    // doesn't report the window going full screen.
    key(c, Qt::Key_F11);
    CHECK(c.w->fullScreenView());
    CHECK(!timelinePane->isVisible());
    key(c, Qt::Key_Escape);
    CHECK(!c.w->fullScreenView());
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

// Two sources whose seam measurement fails for different reasons: each
// clip's status line gives its own reason, not the last failure's, in plain
// words (the decoder's text is for the log).
void testMeasurementFailureReasons(const Ctx &c)
{
    std::printf("  measurement failure reasons\n");
    Project project;
    Timeline *t = project.timeline();
    const int garbage = t->appendClip(c.clipA, kClipMs, kFps); // not a video at all
    const int flat = t->appendClip(c.mp4, kClipMs, kFps);      // a video, not a 360 one
    ClipMeasurements measurements(&project, true);
    const auto status = [&](int id) { return measurements.seamStatus(t->clipById(id)); };
    measurements.schedule();
    CHECK(waitFor([&] {
        return status(garbage).contains(QStringLiteral("couldn't align")) &&
               status(flat).contains(QStringLiteral("couldn't align"));
    }, 20000));
    CHECK(status(flat) == QStringLiteral("Seam: couldn't align this clip (not a 360 camera file)."));
    CHECK(status(garbage) ==
          QStringLiteral("Seam: couldn't align this clip (the file can't be read)."));
}

// A grade pasted from elsewhere (another project's LUT) brings its LUT into
// this project's luts/ folder, as every other way a LUT reaches a clip does.
void testPasteGradeFromElsewhere(const Ctx &c)
{
    auto *copy = find<QPushButton>(c.w, "colourCopy");
    auto *paste = find<QPushButton>(c.w, "colourPaste");
    if (!copy || !paste || c.timeline->count() < 2)
        return;
    const QString outside = QDir(c.dir).filePath(QStringLiteral("Other/luts/Look.cube"));
    QDir().mkpath(QFileInfo(outside).absolutePath());
    {
        QFile f(outside);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("LUT_3D_SIZE 2\n");
        for (int i = 0; i < 8; ++i)
            f.write(QByteArray::number(i & 1) + ' ' + QByteArray::number((i >> 1) & 1) + ' ' +
                    QByteArray::number((i >> 2) & 1) + '\n');
    }
    ColorGrade g = c.timeline->clipAt(0).color;
    g.lut = outside;
    c.timeline->setClipColor(c.timeline->clipAt(0).id, g);
    CHECK(seekTo(c, 500));
    copy->click();
    CHECK(seekTo(c, c.timeline->clipToTimeline(1, 0) + 500));
    settle();
    paste->click();
    const QString pasted = c.timeline->clipAt(1).color.lut;
    CHECK(pasted == QDir(c.dir).filePath(QStringLiteral("Ride/luts/Look.cube")));
    CHECK(QFile::exists(pasted));
    for (int i = 0; i < 2; ++i) {
        ColorGrade none = c.timeline->clipAt(i).color;
        none.lut.clear();
        c.timeline->setClipColor(c.timeline->clipAt(i).id, none);
    }
    settle();
}

// Load LUT... takes a camera maker's zip as downloaded: with several .cube
// files in it the user picks one (the largest Rec.709 one offered first), and
// it is unpacked into the project's luts/ folder.
void testLoadLutFromZip(const Ctx &c)
{
    auto *lut = find<QComboBox>(c.w, "colourLut");
    QAction *undo = action(c, "undoAction");
    if (!lut || !undo || c.timeline->count() < 1)
        return;
    std::string cube = "LUT_3D_SIZE 2\n";
    for (int i = 0; i < 8; ++i)
        cube += std::to_string(i & 1) + ' ' + std::to_string((i >> 1) & 1) + ' ' +
                std::to_string((i >> 2) & 1) + '\n';
    const QString zipFile = QDir(c.dir).filePath(QStringLiteral("Downloads/Cam_LUTs.zip"));
    QDir().mkpath(QFileInfo(zipFile).absolutePath());
    {
        const std::string zip = testzip::makeZip(
            {{"Cam_LUTs/Cam_to_HLG_s65.cube", "# a larger one\n# really\n" + cube},
             {"__MACOSX/Cam_LUTs/._Cam_to_Rec709_s33.cube", "x"},
             {"Cam_LUTs/Cam_to_Rec709_s33.cube", cube}});
        QFile f(zipFile);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(zip.data(), qint64(zip.size()));
    }
    CHECK(seekTo(c, 500));
    settle();
    QStringList offered;
    QString first;
    whenDialog([&](QWidget *w) {
        auto *files = qobject_cast<QFileDialog *>(w);
        CHECK(files);
        if (!files) {
            w->close();
            return;
        }
        files->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))->setText(zipFile);
        whenDialog([&](QWidget *w) {
            auto *pick = qobject_cast<QInputDialog *>(w);
            CHECK(pick);
            if (!pick) {
                w->close();
                return;
            }
            offered = pick->comboBoxItems();
            first = pick->textValue();
            pick->accept();
        });
        static_cast<QDialog *>(files)->accept();
    });
    const int load = lut->count() - 1; // the picker's last item
    lut->setCurrentIndex(load);
    emit lut->activated(load);
    CHECK(offered == QStringList({QStringLiteral("Cam_to_HLG_s65.cube"),
                                  QStringLiteral("Cam_to_Rec709_s33.cube")}));
    CHECK(first == QStringLiteral("Cam_to_Rec709_s33.cube"));
    const QString picked = c.timeline->clipAt(0).color.lut;
    CHECK(picked == QDir(c.dir).filePath(QStringLiteral("Ride/luts/Cam_to_Rec709_s33.cube")));
    CHECK(QFile::exists(picked));
    CHECK(lut->currentText() == QStringLiteral("Cam_to_Rec709_s33"));
    undo->trigger();
    CHECK(c.timeline->clipAt(0).color.lut.isEmpty());
    settle();
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

// Playback goes on past a missing clip (black for its span, a notice naming
// the file) to the clip after it, instead of stopping at the cut; a seek into
// it plays too.
void testPlayPastMissingClip(const Ctx &c)
{
    if (c.timeline->count() != 2)
        return;
    const int third = c.timeline->appendClip(c.clipA, kClipMs, kFps);
    const qint64 cut1 = c.timeline->clipAt(0).timelineDurationMs();
    const qint64 cut2 = cut1 + c.timeline->clipAt(1).timelineDurationMs();
    auto *play = find<QToolButton>(c.w, "playerPlayButton");
    auto *notice = find<QLabel>(c.w, "editNotice");
    if (play && notice) {
        CHECK(seekTo(c, cut1 - 200));
        play->click();
        CHECK(waitFor([&] { return c.timelineWidget->playhead() > cut1 + 100; }));
        const int served = g_framesServed.load(); // the missing clip's are blank
        CHECK(waitFor([&] { return c.timelineWidget->playhead() > cut2 + 300; }, 10000));
        play->click();
        CHECK(g_framesServed.load() > served); // the third clip's frames decoded
        CHECK(notice->text().contains(QFileInfo(c.clipB).fileName()));

        CHECK(seekTo(c, cut1 + 600));
        play->click();
        CHECK(waitFor([&] { return c.timelineWidget->playhead() > cut1 + 800; }));
        play->click();
    }
    c.timeline->removeClip(third);
}

// A clip's file moved: it shows as missing, and Locate file… relinks it.
void testLocateMovedFile(const Ctx &c)
{
    const QString moved = QDir(c.dir).filePath(QStringLiteral("moved.insv"));
    CHECK(QFile::rename(c.clipB, moved));
    answerMessageBox(QStringLiteral("OK")); // the missing-media warning
    c.w->openProject(c.project->filePath());
    CHECK(waitFor([&] { return c.timeline->count() == 2 && c.timeline->clipAt(1).mediaMissing; }));
    testPlayPastMissingClip(c);

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
    // The first clip split: the panels show its split on opening, before
    // anything is clicked.
    const pane::Split before = c.timeline->clipAt(0).split;
    pane::Split grid;
    grid.layout = pane::Layout::Grid;
    c.timeline->setClipSplit(c.timeline->clipAt(0).id, grid, 0);
    CHECK(c.project->modified());
    trigger(c, "saveProjectAction");
    CHECK(!c.project->modified());

    MainWindow again(nullptr, standIns());
    auto *project2 = again.findChild<Project *>();
    again.openProject(c.orp);
    CHECK(project2 && project2->timeline()->count() == c.timeline->count());
    CHECK(project2 && project2->timeline()->trackCount() == tracks + 1);
    CHECK(project2 && !project2->modified());
    auto *layout = find<QComboBox>(&again, "splitLayoutCombo");
    auto *paneCombo = find<QComboBox>(&again, "activePaneCombo");
    CHECK(layout && layout->currentData().toInt() == int(pane::Layout::Grid) &&
          layout->isEnabledTo(&again));
    CHECK(paneCombo && paneCombo->count() == 4);

    c.timeline->setClipSplit(c.timeline->clipAt(0).id, before, 0);
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

// A new project starts from scratch: the settings shown for the last
// project's clip, its view direction and the reframe arm don't carry over.
void testNewProjectResets(const Ctx &c)
{
    auto *exposure = find<QSlider>(c.w, "colourExposure");
    auto *mode = find<QComboBox>(c.w, "stabilizationMode");
    auto *arm = find<QToolButton>(c.w, "reframeArmButton");
    if (!exposure || !mode || !arm)
        return;
    c.w->openProject(c.orp);
    CHECK(c.timeline->count() > 0);
    settle();
    CHECK(seekTo(c, 500));
    const int clipId = c.timeline->clipAt(0).id;
    c.timeline->setClipStabilization(clipId,
                                     insv::StabConfig::preset(insv::StabMode::LeanFollow));
    exposure->setValue(10); // +1.0 EV
    CHECK(waitFor([&] { return mode->currentIndex() != 0; }));
    arm->setChecked(true);
    c.w->viewport()->SetPose({1.0f, 0.3f, 0.1f, 1.0f});
    CHECK(c.project->save(c.project->filePath())); // nothing to ask about leaving

    fillProjectDialog(c, QStringLiteral("Third"));
    trigger(c, "newProjectAction");
    CHECK(c.timeline->count() == 0);
    CHECK(exposure->value() == 0 && !exposure->isEnabled());
    CHECK(mode->currentIndex() == 0);
    CHECK(!arm->isChecked());
    const EquirectViewportWindow *v = c.w->viewport();
    const ReframePose ahead;
    CHECK(v->Yaw() == ahead.yaw && v->Pitch() == ahead.pitch && v->Roll() == ahead.roll &&
          v->Pose().fov == ahead.fov && v->Pose().distortion == ahead.distortion);
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
    c.hlgMp4 = home.filePath(QStringLiteral("phone-hlg.mp4"));
    c.orp = home.filePath(QStringLiteral("test.orp"));
    writeFile(c.clipA);
    writeFile(c.clipB);
    testmedia::writeWav(c.wav.toStdString(), 3 * testmedia::kWavRate, [](int i) {
        return 0.3 * std::sin(i * 2.0 * M_PI * 440.0 / testmedia::kWavRate);
    });
    testmedia::VideoSpec video;
    video.luma = [](int frame, int, int) { return uint8_t(40 + frame * 3); };
    CHECK(testmedia::writeVideo(c.mp4.toStdString(), video));
    video.hlg = true;
    CHECK(testmedia::writeVideo(c.hlgMp4.toStdString(), video));

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
            testKeyOnTrimOut(c);
            testStepOntoCut(c);
            testVideoEdits(c);
            testColour(c);
            testStitching(c);
            testStitchingWithoutGuards(c);
            testDirectionLock(c);
            testSplitScreen(c);
            testDuplicateClip(c);
            testMediaPool(c);
            testTakeFiles(c);
            testAudioTrack(c);
            testOverlay(c);
            testOverlayLayers(c);
            testFlatMainClip(c);
            testHlgClip(c);
            testTitle(c);
            testTitlePresets(c);
            testTransition(c);
            testFullScreen(c);
            testTrackSubjectMode(c);
            testViewMenu(c);
            testHelpAndSettingsDialogs(c);
            testSaveAs(c);
            testPasteGradeFromElsewhere(c);
            testLoadLutFromZip(c);
            testMeasurementFailureReasons(c);
            testMenuFileDialogs(c);
            testRecovery(c);
            testLocateMovedFile(c);
            testSaveAndReopen(c);
            testCloseWithChanges(c);
            testNewProject(c);
            testNewProjectResets(c);
        }
    }

    if (g_failures == 0)
        std::printf("main_window_test: all checks passed\n");
    else
        std::printf("main_window_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
