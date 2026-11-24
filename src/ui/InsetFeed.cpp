#include "InsetFeed.h"

#include <FlatVideoReader.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

// Frames decoded ahead of the playhead per clip while playing.
constexpr size_t kAhead = 4;
// Decode pictures no bigger than this on the long side: an inset rarely
// covers more of the view, and it keeps the per-frame upload small.
constexpr int kMaxSide = 1280;
// A flat main-track clip fills the whole view: as large as a full-HD view.
constexpr int kFullFrameMaxSide = 1920;
// Start decoding a clip this long before it appears (the first frame then
// shows on time), and let go of its decoder this long after.
constexpr qint64 kPrerollMs = 1500;
constexpr qint64 kLingerMs = 500;
// While playing, how far the clock may run past the newest decoded frame
// before the decode is re-anchored (a seek) rather than waited for.
constexpr qint64 kBehindMs = 500;
// How far past its span a transition's layer still shows over its main
// clip (Clip::overMainClipId): the main picture lagging the clock.
constexpr qint64 kOverMainSlackMs = 250;
// A read that failed with an error rather than at the clip's end is tried
// again after this long, playing / paused, as the main clip's are; the
// picture shown stays up meanwhile. An inset has no next clip to go on
// with, so it keeps trying while it's near the playhead: a frame that
// never reads is passed when the playhead moves on and the decode is
// re-anchored after it.
constexpr std::chrono::milliseconds kReadRetryPlaying{100};
constexpr std::chrono::milliseconds kReadRetryPaused{1000};

} // namespace

struct InsetFeed::Stream {
    Clip clip; // guarded by m_mx
    // --- guarded by m_mx ---
    std::deque<Frame> queue; // front = showing (or next to)
    bool eof = false;
    bool needAnchor = true;
    // A title's picture showing, and its frame key.
    exportpipe::Still still;
    uint64_t stillKey = 0;
    uint64_t gen = 0; // bumped on every re-anchor request
    bool failed = false;
    bool removed = false;
    bool reopen = false; // close the reader / picture before the next step
    bool waiting = false; // its picture isn't ready to read yet: until poke()
    // The last read failed with an error, not at the end: the next one waits
    // until `retryAt`.
    bool readFailed = false;
    std::chrono::steady_clock::time_point retryAt;
    // Anchored at a frame its picture left out for its blur
    // (exportpipe::PictureSource::refused), at this timeline time, in
    // re-anchor `refusedGen`: the picture before stays up (parked()).
    qint64 refusedMs = -1;
    uint64_t refusedGen = 0;
    Frame shown;
    bool haveShown = false;
    double frameMs = 0.0; // set when the reader opens
    // --- decoder thread only ---
    std::unique_ptr<exportpipe::FlatVideoReader> reader;
    std::unique_ptr<exportpipe::PictureSource> picture; // instead, for a 360 view
    QString openedPath; // or the picture's key
    bool saidRefused = false; // logged that its picture leaves frames out
    bool open() const { return reader || picture; }
    // At `ms` (`playing` or not), hold the picture shown rather than
    // re-anchor: the anchor's frame was left out. Paused, until the playhead
    // moves -- the frames after it aren't the playhead's, and re-anchoring
    // would only refuse it again; playing, while the frames read on from it
    // come, as the main clip's do.
    bool parked(qint64 ms, bool playing) const
    {
        return refusedMs >= 0 && refusedGen == gen &&
               (playing ? ms < refusedMs + kBehindMs : ms == refusedMs);
    }
    void close()
    {
        reader.reset();
        picture.reset();
    }
};

InsetFeed::InsetFeed()
{
    m_thread = std::thread([this] { decoderLoop(); });
}

InsetFeed::~InsetFeed()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void InsetFeed::setClips(std::vector<Clip> clips)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        std::vector<std::shared_ptr<Stream>> next;
        for (Clip &c : clips) {
            std::shared_ptr<Stream> s;
            for (const auto &o : m_streams)
                if (o->clip.id == c.id)
                    s = o;
            if (!s) {
                s = std::make_shared<Stream>();
            } else if (!s->clip.sameFrames(c)) {
                // Trimmed / moved / re-sourced: the queued frames are wrong.
                s->queue.clear();
                s->eof = false;
                s->needAnchor = true;
                s->waiting = false;
                ++s->gen;
                if (s->clip.path != c.path || s->clip.pictureKey != c.pictureKey)
                    s->failed = s->readFailed = false;
            }
            s->clip = std::move(c);
            next.push_back(std::move(s));
        }
        for (const auto &o : m_streams)
            if (std::find(next.begin(), next.end(), o) == next.end())
                o->removed = true;
        m_streams = std::move(next);
    }
    m_cv.notify_all();
}

void InsetFeed::reopen(const QStringList &paths)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        for (const auto &s : m_streams) {
            if (s->clip.path.isEmpty() || !paths.contains(s->clip.path))
                continue;
            s->reopen = true;
            s->failed = s->readFailed = false;
            s->queue.clear();
            s->eof = false;
            s->needAnchor = true;
            s->waiting = false;
            ++s->gen;
        }
    }
    m_cv.notify_all();
}

void InsetFeed::poke()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        for (const auto &s : m_streams)
            s->waiting = false;
    }
    m_cv.notify_all();
}

bool InsetFeed::active(const Clip &c, qint64 ms) const
{
    return ms >= c.startMs - kPrerollMs && ms < c.endMs() + kLingerMs;
}

namespace {

// A still's clip-local time to have ready for timeline time `ms`, and the
// one after it.
std::pair<qint64, qint64> stillTimes(const InsetFeed::Clip &c, qint64 ms)
{
    const qint64 now = std::clamp(ms - c.startMs, qint64(0), std::max(qint64(0), c.spanMs() - 1));
    return {now, c.still->nextChange(now)};
}

// Does a still want a picture made?
bool stillWants(const InsetFeed::Clip &c, qint64 ms)
{
    const auto [now, next] = stillTimes(c, ms);
    return c.still->ready(now).empty() || (next >= 0 && c.still->ready(next).empty());
}

} // namespace

std::vector<EquirectViewportWindow::InsetDraw> InsetFeed::frameFor(qint64 clockMs, bool playing,
                                                                   int mainClipId)
{
    std::vector<EquirectViewportWindow::InsetDraw> out;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        wake = clockMs != m_targetMs || playing != m_playing;
        m_targetMs = clockMs;
        m_playing = playing;
        for (const auto &sp : m_streams) {
            Stream &s = *sp;
            const Clip &c = s.clip;
            qint64 ms = clockMs;
            if (c.overMainClipId) {
                if (c.overMainClipId != mainClipId || ms < c.startMs - kOverMainSlackMs ||
                    ms >= c.endMs() + kOverMainSlackMs) {
                    s.haveShown = false;
                    continue;
                }
                ms = std::clamp(ms, c.startMs, std::max(c.startMs, c.endMs() - 1));
            }
            if (ms < c.startMs || ms >= c.endMs()) {
                s.haveShown = false;
                continue;
            }
            if (c.still) {
                // The picture for now when it's ready, else the last one
                // (the decoder thread is making it).
                const exportpipe::Still now = c.still->ready(ms - c.startMs);
                if (now.empty()) {
                    wake = true;
                } else if (now.rgba != s.still.rgba) {
                    s.still = now;
                    s.stillKey = m_nextKey++; // a new picture: uploaded again
                }
                if (s.still.empty())
                    continue;
                const exportpipe::Still &pic = s.still;
                EquirectViewportWindow::InsetDraw d;
                d.clipId = c.id;
                d.frameKey = s.stillKey;
                d.rgba = pic.rgba;
                d.width = pic.width;
                d.height = pic.height;
                const inset::Look look = c.keys.isEmpty() ? c.look : c.keys.sample(c.sourceAt(ms));
                d.aspect = pic.widthPerSize > 0.0 ? double(pic.width) / std::max(1, pic.height)
                                                  : c.aspect;
                d.look = pic.widthPerSize > 0.0
                             ? inset::anchored(look, pic.widthPerSize, pic.anchorX)
                             : look;
                d.look = inset::moved(d.look, c.motion, ms - c.startMs, c.spanMs(), d.aspect,
                                      c.outAspect);
                d.anchorDx = d.look.centerX - look.centerX;
                d.anchorDy = d.look.centerY - look.centerY;
                d.look.opacity *= inset::fadeGain(ms - c.startMs, c.spanMs(), c.fadeInMs, c.fadeOutMs) *
                                  inset::rampGain(ms - c.startMs, c.spanMs(), c.rampFrom, c.rampTo);
                d.locked = c.fullFrame;
                out.push_back(std::move(d));
                continue;
            }
            // The frame whose window [time - half, time + half) holds `ms`,
            // as the export picks a layer's (FlatVideoReader::frameAtUs).
            const double tol = s.frameMs / 2.0;
            const size_t before = s.queue.size();
            while (s.queue.size() >= 2 && s.queue[1].timelineMs <= ms + tol)
                s.queue.pop_front();
            wake |= s.queue.size() != before;
            // Does the queue hold the frame for `ms`? The first frame of a
            // clip may start a hair after the clip on the timeline.
            bool covered = false;
            if (!s.queue.empty() && s.queue.front().timelineMs <= ms + tol) {
                const qint64 slack = playing ? kBehindMs : std::llround(s.frameMs * 1.5);
                covered = s.queue.size() >= 2 || s.eof || ms < s.queue.front().timelineMs + slack;
            }
            if (!covered && !s.needAnchor && !s.failed && !s.parked(ms, playing)) {
                s.queue.clear();
                s.eof = false;
                s.needAnchor = true;
                ++s.gen;
                wake = true;
            }
            if (covered) {
                s.shown = s.queue.front();
                s.haveShown = true;
            }
            if (!s.haveShown)
                continue; // nothing decoded for it yet
            const qint64 local = ms - c.startMs;
            EquirectViewportWindow::InsetDraw d;
            d.clipId = c.id;
            d.frameKey = s.shown.key;
            d.rgba = s.shown.rgba;
            d.width = s.shown.width;
            d.height = s.shown.height;
            const inset::Look look = c.keys.isEmpty() ? c.look : c.keys.sample(c.sourceAt(ms));
            d.look = inset::moved(look, c.motion, local, c.spanMs(), c.aspect, c.outAspect);
            d.anchorDx = d.look.centerX - look.centerX;
            d.anchorDy = d.look.centerY - look.centerY;
            d.look.opacity *= inset::fadeGain(local, c.spanMs(), c.fadeInMs, c.fadeOutMs) *
                              inset::rampGain(local, c.spanMs(), c.rampFrom, c.rampTo);
            d.frame = c.frame;
            d.aspect = c.aspect;
            d.colorGrade = c.colorGrade;
            d.locked = c.fullFrame;
            out.push_back(std::move(d));
        }
    }
    if (wake)
        m_cv.notify_all();
    return out;
}

bool InsetFeed::work(Stream &s, qint64 target, bool playing)
{
    Clip clip;
    uint64_t gen = 0;
    bool anchor = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        clip = s.clip;
        gen = s.gen;
        anchor = s.needAnchor;
        if (s.removed || s.failed || s.waiting || !active(clip, target) ||
            (s.readFailed && std::chrono::steady_clock::now() < s.retryAt))
            return false;
        if (!clip.still && !anchor && (!playing || s.eof || s.queue.size() >= kAhead))
            return false;
        if (std::exchange(s.reopen, false))
            s.close();
    }

    // A title: make the picture for now, then the next one.
    if (clip.still) {
        const auto [now, next] = stillTimes(clip, target);
        if (clip.still->ready(now).empty())
            clip.still->at(now);
        else if (next >= 0 && clip.still->ready(next).empty())
            clip.still->at(next);
        else
            return false;
        return true;
    }

    // A 360 clip's view: open (or re-open, if what it renders changed) its
    // picture source, at the size of a full-frame picture of its shape.
    if (clip.picture && (!s.picture || s.openedPath != clip.pictureKey)) {
        s.close();
        s.picture = clip.picture();
        s.openedPath = clip.pictureKey;
        std::string err;
        if (!s.picture || !s.picture->open(err)) {
            if (!s.picture)
                err = "no picture source: " + clip.path.toStdString();
            std::fprintf(stderr, "[inset] %s\n", err.c_str()); // names the file
            s.picture.reset();
            std::lock_guard<std::mutex> lk(m_mx);
            s.failed = true;
            return true;
        }
        const double aspect = std::max(0.1, clip.aspect);
        const int w = aspect >= 1.0 ? kFullFrameMaxSide : int(std::lround(kFullFrameMaxSide * aspect));
        s.picture->setOutputSize(std::max(4, w & ~3),
                                 std::max(2, int(std::lround(w / aspect)) & ~1));
        std::lock_guard<std::mutex> lk(m_mx);
        s.frameMs = 1000.0 / std::max(1.0, s.picture->fps());
    }
    // Open (or re-open, if the clip was re-sourced) the reader.
    if (!clip.picture && (!s.reader || s.openedPath != clip.path)) {
        s.close();
        s.reader = std::make_unique<exportpipe::FlatVideoReader>();
        s.openedPath = clip.path;
        std::string err;
        if (!s.reader->open(clip.path.toStdString(), err)) {
            std::fprintf(stderr, "[inset] %s\n", err.c_str());
            s.reader.reset();
            std::lock_guard<std::mutex> lk(m_mx);
            s.failed = true;
            return true;
        }
        const auto &info = s.reader->info();
        const double scale = std::min(1.0, double(clip.fullFrame ? kFullFrameMaxSide : kMaxSide) /
                                               std::max(info.width, info.height));
        s.reader->setOutputSize(std::max(2, int(std::lround(info.width * scale)) & ~1),
                                std::max(2, int(std::lround(info.height * scale)) & ~1));
        std::lock_guard<std::mutex> lk(m_mx);
        s.frameMs = 1000.0 / info.fps;
    }

    Frame f;
    bool got = false;
    bool ended = false;
    bool waiting = false;
    // A frame left out for its blur: no picture, not the end either -- the
    // source is past it, and the next read gives the next frame.
    bool refused = false;
    // A read that failed with an error: why. Not the end -- tried again.
    std::string failure;
    const auto readError = [&s] {
        return s.picture ? s.picture->lastError() : s.reader->lastError();
    };
    const qint64 at = std::clamp(target, clip.startMs, std::max(clip.startMs, clip.endMs() - 1));
    if (anchor) {
        // The clock is in whole ms (playback isn't on a frame grid).
        const qint64 srcUs = clip.sourceAt(at) * 1000;
        if (const exportpipe::VideoFrame *v = s.picture ? s.picture->frameAtUs(srcUs)
                                                        : s.reader->frameAtUs(srcUs)) {
            f.timelineMs = clip.timelineAt(v->ptsMs);
            f.width = v->width;
            f.height = v->height;
            f.rgba = std::make_shared<const std::vector<uint8_t>>(v->rgba);
            got = true;
        } else if (s.picture && s.picture->waiting()) {
            waiting = true;
        } else if (s.picture && s.picture->refused()) {
            refused = true;
        } else {
            failure = readError();
            ended = failure.empty();
        }
    } else {
        exportpipe::VideoFrame v;
        const bool read = s.picture ? s.picture->readNext(v) : s.reader->readNext(v);
        if (read && clip.timelineAt(v.ptsMs) < clip.endMs()) {
            f.timelineMs = clip.timelineAt(v.ptsMs);
            f.width = v.width;
            f.height = v.height;
            f.rgba = std::make_shared<const std::vector<uint8_t>>(std::move(v.rgba));
            got = true;
        } else if (!read && s.picture && s.picture->refused()) {
            refused = true;
        } else {
            failure = read ? std::string() : readError();
            ended = failure.empty();
        }
    }
    if (refused && !std::exchange(s.saidRefused, true))
        std::fprintf(stderr, "[inset] %s: frames with more privacy regions than the blur takes "
                             "are left out (%s)\n",
                     qPrintable(clip.path), s.picture->lastError().c_str());

    bool firstFailure = false;
    bool readsAgain = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (s.gen != gen || s.removed)
            return true; // re-anchored or removed meanwhile: drop it
        if (waiting) {
            s.waiting = true; // the anchor stays wanted
            return true;
        }
        if (!failure.empty()) {
            // The anchor stays wanted, or the read goes on from the failed
            // frame; the picture shown stays up.
            firstFailure = !std::exchange(s.readFailed, true);
            s.retryAt = std::chrono::steady_clock::now() +
                        (playing ? kReadRetryPlaying : kReadRetryPaused);
        } else {
            readsAgain = got && std::exchange(s.readFailed, false);
            if (anchor) {
                s.needAnchor = false;
                s.queue.clear();
                s.eof = ended;
                if (refused) {
                    s.refusedMs = at;
                    s.refusedGen = s.gen;
                }
            } else if (ended) {
                s.eof = true;
            }
            if (got) {
                s.refusedMs = -1;
                f.key = m_nextKey++;
                s.queue.push_back(std::move(f));
            }
        }
    }
    if (firstFailure)
        std::fprintf(stderr, "[inset] a frame of %s didn't read (%s); trying it again\n",
                     qPrintable(clip.path), failure.c_str());
    if (readsAgain)
        std::fprintf(stderr, "[inset] %s reads again\n", qPrintable(clip.path));
    return true;
}

void InsetFeed::decoderLoop()
{
    for (;;) {
        std::vector<std::shared_ptr<Stream>> streams;
        qint64 target = 0;
        bool playing = false;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            // The earliest retry of a failed read, when that's all there is.
            auto retry = std::chrono::steady_clock::time_point::max();
            const auto ready = [this, &retry] {
                if (m_stop)
                    return true;
                retry = std::chrono::steady_clock::time_point::max();
                const auto now = std::chrono::steady_clock::now();
                for (const auto &s : m_streams) {
                    const bool live = !s->removed && !s->failed && active(s->clip, m_targetMs);
                    if (!live && s->open())
                        return true; // a decoder to let go of
                    if (s->waiting)
                        continue;
                    if (live && s->readFailed && now < s->retryAt) {
                        retry = std::min(retry, s->retryAt);
                        continue;
                    }
                    if (live && s->clip.still) {
                        if (stillWants(s->clip, m_targetMs))
                            return true;
                        continue;
                    }
                    if (live && (s->needAnchor ||
                                 (m_playing && !s->eof && s->queue.size() < kAhead)))
                        return true;
                }
                return false;
            };
            while (!ready()) {
                if (retry == std::chrono::steady_clock::time_point::max())
                    m_cv.wait(lk);
                else
                    m_cv.wait_until(lk, retry);
            }
            if (m_stop)
                return;
            streams = m_streams;
            target = m_targetMs;
            playing = m_playing;
        }
        // Re-anchors first (a seek wants its frame now), then top up the
        // queues. One step each per pass, so no clip starves another.
        bool did = false;
        for (const auto &s : streams) {
            bool live = false;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                live = !s->removed && !s->failed && active(s->clip, target);
                if (!live && s->open()) {
                    s->queue.clear();
                    s->needAnchor = true;
                    s->waiting = false;
                    ++s->gen;
                }
            }
            if (!live) {
                s->close(); // free the decoder
                continue;
            }
            did |= work(*s, target, playing);
        }
        if (!did)
            std::this_thread::yield();
    }
}
