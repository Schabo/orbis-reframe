#include "OverlayText.h"

#include <QCoreApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QTextLayout>
#include <QThread>

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>

namespace overlaytext {

namespace {

struct TextThread {
    std::mutex mx;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    QThread *thread = nullptr;
    bool stopped = false; // shut down with the application
};

TextThread &textThread()
{
    // Never destroyed: the thread is stopped before the application goes,
    // and a call after that runs on its caller.
    static TextThread *t = new TextThread;
    return *t;
}

void stopTextThread()
{
    TextThread &t = textThread();
    QThread *thread = nullptr;
    {
        std::lock_guard<std::mutex> lk(t.mx);
        t.stopped = true;
        thread = std::exchange(t.thread, nullptr);
    }
    t.wake.notify_all();
    if (thread) {
        thread->wait(); // the jobs already queued are done first
        delete thread;
    }
}

void loop(TextThread &t)
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(t.mx);
            t.wake.wait(lk, [&] { return t.stopped || !t.jobs.empty(); });
            if (t.jobs.empty())
                return; // stopped
            job = std::move(t.jobs.front());
            t.jobs.pop_front();
        }
        job();
    }
}

// A font whose glyphs are pictures (colour emoji: bitmaps, layers, SVG),
// not outlines to fill. Known by its tables, looked at once per font (an
// emoji font's are megabytes).
bool isColourFont(const QRawFont &raw)
{
    static std::mutex mx;
    static QHash<QString, bool> known;
    const QString key = raw.familyName() + QLatin1Char('\n') + raw.styleName();
    std::lock_guard<std::mutex> lk(mx);
    if (const auto it = known.constFind(key); it != known.constEnd())
        return *it;
    bool colour = false;
    for (const char *table : {"CBDT", "sbix", "COLR", "SVG "})
        colour = colour || !raw.fontTable(table).isEmpty();
    known.insert(key, colour);
    return colour;
}

} // namespace

void run(const std::function<void()> &job)
{
    TextThread &t = textThread();
    std::mutex doneMx;
    std::condition_variable doneCv;
    bool done = false;
    bool queued = false;
    std::exception_ptr failed;
    {
        std::lock_guard<std::mutex> lk(t.mx);
        if (!t.stopped && !(t.thread && QThread::currentThread() == t.thread)) {
            if (!t.thread) {
                t.thread = QThread::create([&t] { loop(t); });
                t.thread->setObjectName(QStringLiteral("overlay text"));
                t.thread->start();
                // Stopped while Qt's fonts are still there; its FreeType
                // faces go with the thread.
                qAddPostRoutine(stopTextThread);
            }
            t.jobs.push_back([&] {
                try {
                    job();
                } catch (...) {
                    failed = std::current_exception(); // thrown on in the caller
                }
                std::lock_guard<std::mutex> dl(doneMx);
                done = true;
                doneCv.notify_one();
            });
            queued = true;
        }
    }
    if (!queued) {
        job(); // on the text thread already, or shutting down
        return;
    }
    t.wake.notify_one();
    std::unique_lock<std::mutex> dl(doneMx);
    doneCv.wait(dl, [&] { return done; });
    if (failed)
        std::rethrow_exception(failed);
}

void fillGlyphRuns(QPainter &p, const QList<QGlyphRun> &runs, QPointF offset)
{
    QPainterPath outline;
    outline.setFillRule(Qt::WindingFill);
    for (const QGlyphRun &run : runs) {
        const QRawFont raw = run.rawFont();
        if (isColourFont(raw)) {
            p.drawGlyphRun(offset, run);
            continue;
        }
        const QList<quint32> glyphs = run.glyphIndexes();
        const QList<QPointF> at = run.positions();
        for (qsizetype g = 0; g < glyphs.size(); ++g)
            outline.addPath(raw.pathForGlyph(glyphs[g]).translated(at[g] + offset));
    }
    p.fillPath(outline, p.pen().brush());
}

void drawText(QPainter &p, QPointF baseline, const QString &text)
{
    QTextLayout layout(text, p.font());
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid())
        line.setPosition(QPointF(0.0, 0.0));
    layout.endLayout();
    if (!line.isValid())
        return;
    fillGlyphRuns(p, line.glyphRuns(), baseline - QPointF(0.0, line.ascent()));
}

} // namespace overlaytext
