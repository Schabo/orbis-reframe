// The whole main window on the offscreen platform, without a GPU: no Vulkan
// instance (the viewport keeps its state but never renders) and stand-ins
// for the clip probe and the stitch source (ClipBackend), so "camera files"
// are any file named *.insv and frames are flat grey. Driven through the
// actions and widgets found by objectName: the start screen, opening a clip,
// playback reaching the viewport, an undoable edit, saving and reopening the
// project. Exit code is the failure count.

#include "ClipBackend.h"
#include "MainWindow.h"
#include "Theme.h"

#include "../core/Project.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSplitter>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <atomic>
#include <cstdio>

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

// Flat grey equirect frames, small, one per 1/30 s.
class StandInSource : public stitch::IStitchSource {
public:
    bool Open(const std::string &path, const stitch::PreviewStitchConfig &) override
    {
        if (!QFile::exists(QString::fromStdString(path))) {
            m_error = "no such file";
            return false;
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

QAction *action(MainWindow &w, const char *name) { return find<QAction>(&w, name); }

void testStartScreen(MainWindow &w)
{
    auto *pages = w.findChild<QStackedWidget *>();
    CHECK(pages);
    CHECK(pages && pages->currentWidget() == w.findChild<QWidget *>(QStringLiteral("projectLibrary")));
    // Editor-only actions wait for a project.
    if (QAction *a = action(w, "saveProjectAction"))
        CHECK(!a->isEnabled());
    if (QAction *a = action(w, "addAudioTrackAction"))
        CHECK(!a->isEnabled());
}

void testOpenClip(MainWindow &w, Project *project, const QString &clip)
{
    w.openClip(clip);
    auto *pages = w.findChild<QStackedWidget *>();
    CHECK(pages && pages->currentWidget() == find<QSplitter>(&w, "editorSplitter"));
    CHECK(project->timeline()->count() == 1);
    if (project->timeline()->count() == 1)
        CHECK(project->timeline()->clipAt(0).sourceDurationMs == kClipMs);
    if (QAction *a = action(w, "saveProjectAction"))
        CHECK(a->isEnabled());
}

// The stand-in source is opened on the playback thread and its frames reach
// the viewport, which keeps the latest one without a renderer to take it.
void testPlayback(MainWindow &w)
{
    EquirectViewportWindow *viewport = w.viewport();
    CHECK(waitFor([] { return g_framesServed.load() > 0; }));
    auto *play = find<QToolButton>(&w, "playerPlayButton");
    auto *time = find<QToolButton>(&w, "playerTime");
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

void testUndo(MainWindow &w, Project *project)
{
    QAction *addTrack = action(w, "addAudioTrackAction");
    QAction *undo = action(w, "undoAction");
    QAction *redo = action(w, "redoAction");
    if (!addTrack || !undo || !redo)
        return;
    const int tracks = project->timeline()->trackCount();
    addTrack->trigger();
    CHECK(project->timeline()->trackCount() == tracks + 1);
    CHECK(project->modified());
    CHECK(undo->isEnabled());
    undo->trigger();
    CHECK(project->timeline()->trackCount() == tracks);
    CHECK(redo->isEnabled());
    redo->trigger();
    CHECK(project->timeline()->trackCount() == tracks + 1);
}

void testSaveAndReopen(MainWindow &w, Project *project, const QString &orp)
{
    // Saved once under a path, then File ▸ Save project saves in place.
    QString error;
    CHECK(project->save(orp, &error));
    const int tracks = project->timeline()->trackCount();
    QAction *addTrack = action(w, "addAudioTrackAction");
    QAction *save = action(w, "saveProjectAction");
    if (!addTrack || !save)
        return;
    addTrack->trigger();
    CHECK(project->modified());
    save->trigger();
    CHECK(!project->modified());

    MainWindow again(nullptr, standIns());
    auto *project2 = again.findChild<Project *>();
    again.openProject(orp);
    CHECK(project2 && project2->timeline()->count() == 1);
    CHECK(project2 && project2->timeline()->trackCount() == tracks + 1);
    CHECK(project2 && !project2->modified());
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // Settings, the project library's list and caches: not the user's.
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());
    qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/data")).toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (home.path() + QStringLiteral("/cache")).toLocal8Bit());
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("orbis-reframe"));
    theme::apply(app, theme::themes().first());

    const QString clip = home.filePath(QStringLiteral("VID_20260101_120000_00_001.insv"));
    writeFile(clip);
    const QString orp = home.filePath(QStringLiteral("test.orp"));

    {
        MainWindow w(nullptr, standIns());
        w.resize(1280, 760);
        w.showLibrary();
        w.show();
        QApplication::processEvents();
        auto *project = w.findChild<Project *>();
        CHECK(project);
        if (project) {
            testStartScreen(w);
            testOpenClip(w, project, clip);
            testPlayback(w);
            testUndo(w, project);
            testSaveAndReopen(w, project, orp);
        }
    }

    if (g_failures == 0)
        std::printf("main_window_test: all checks passed\n");
    else
        std::printf("main_window_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
