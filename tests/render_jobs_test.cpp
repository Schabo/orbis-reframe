// src/ui/RenderJobs: the edit model as playback, the export and a
// screenshot are handed it -- the audio mix, the overlays (what's left out,
// in which order), titles, transitions (their layers and crossfading
// sound), a main-track clip's export job, and the suggested file names. No
// GPU, no media.

#include "RenderJobs.h"

#include "PrivacyBlur.h"
#include "TitleRender.h"
#include "GaugeRender.h"
#include "GaugeSource.h"
#include "TelemetryCache.h"
#include "TitleSource.h"
#include "../core/TitleClock.h"
#include "../core/ClipItem.h"
#include "../core/MediaPool.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QTimeZone>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

bool near(double a, double b, double eps = 1e-4) { return std::abs(a - b) < eps; }

// Where the export draws a title job's picture at clip-local `localMs`
// (before its motion), and that picture.
inset::Rect titleRect(const exportpipe::InsetClipJob &j, qint64 localMs, int outW = 1920,
                      int outH = 1080, exportpipe::Still *still = nullptr)
{
    const exportpipe::Still pic = j.still->at(localMs);
    if (still)
        *still = pic;
    const inset::Look l = inset::anchored(j.lookAt(j.trimInMs + localMs), pic.widthPerSize,
                                          pic.anchorX);
    return inset::place(l, {}, double(pic.width) / pic.height, outW, outH);
}

void testMix()
{
    Timeline t;
    const int music = t.addTrack(Track::Kind::Audio, "Music");
    const int muted = t.addTrack(Track::Kind::Audio, "Muted");
    t.setTrackAudio(music, -6.0, false);
    t.setTrackAudio(muted, 0.0, true);
    const int a = t.addAudioClip(music, "/m/a.wav", 5000, 1000);
    t.setAudioClipLevel(a, -6.0, false);
    t.setTrackClipFades(a, 500, 250);
    const int quiet = t.addAudioClip(music, "/m/quiet.wav", 1000, 8000);
    t.setAudioClipLevel(quiet, 0.0, true);
    t.addAudioClip(music, "/m/gone.wav", 1000, 10000);
    t.addAudioClip(muted, "/m/b.wav", 1000, 0);
    t.setSourcesMissing({{"/m/gone.wav", true}});

    const std::vector<insv::MixClip> mix = renderjobs::mixClips(t);
    CHECK(mix.size() == 1); // muted track, muted clip and missing file left out
    if (mix.size() == 1) {
        const insv::MixClip &m = mix[0];
        CHECK(m.path == "/m/a.wav");
        CHECK(m.startFrame == 1000 * 48 && m.lengthFrames == 5000 * 48 && m.inFrame == 0);
        CHECK(near(m.gain, std::pow(10.0, -12.0 / 20.0))); // clip + track
        CHECK(m.fadeInFrames == 500 * 48 && m.fadeOutFrames == 250 * 48);
    }
}

void testOverlays()
{
    Timeline t;
    const int v1 = t.addTrack(Track::Kind::Video, "V1");
    const int v2 = t.addTrack(Track::Kind::Video, "V2");
    const int hidden = t.addTrack(Track::Kind::Video, "Hidden");
    t.setTrackAudio(hidden, 0.0, true);
    const int c1 = t.addVideoClip(v1, "/v/one.mp4", 4000, 0, 1920, 1080);
    const int c2 = t.addVideoClip(v2, "/v/two.mp4", 4000, 500, 1080, 1920);
    t.addVideoClip(hidden, "/v/hidden.mp4", 4000, 0, 1920, 1080);
    t.addVideoClip(v2, "/v/gone.mp4", 1000, 6000, 1920, 1080);
    t.setSourcesMissing({{"/v/gone.mp4", true}});
    inset::Look keyed;
    keyed.centerX = 0.25f;
    t.upsertTrackClipInsetKey(c2, 1000, keyed);

    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
    CHECK(feed.size() == 2 && jobs.size() == 2);
    if (feed.size() != 2 || jobs.size() != 2)
        return;
    // Bottom-most first: the track listed last draws first.
    const int i1 = t.trackIndexOfId(v1), i2 = t.trackIndexOfId(v2);
    const int first = i1 > i2 ? c1 : c2;
    CHECK(feed[0].id == first);
    CHECK(jobs[0].path == (first == c1 ? "/v/one.mp4" : "/v/two.mp4"));
    for (size_t k = 0; k < feed.size(); ++k) {
        CHECK(jobs[k].path == feed[k].path.toStdString());
        CHECK(jobs[k].startMs == feed[k].startMs && near(jobs[k].aspect, feed[k].aspect));
    }
    const size_t k2 = feed[0].id == c2 ? 0 : 1;
    CHECK(near(feed[k2].aspect, 1080.0 / 1920.0));
    CHECK(near(jobs[k2].lookAt(1000).centerX, 0.25f)); // its key, sampled
    CHECK(jobs[1 - k2].lookAt(1000).centerX != 0.25f); // the other keeps its own look
}

// A title is its text's picture, placed by its text size; blank text
// draws nothing.
void testTitles()
{
    Timeline t;
    t.appendClip("/c/ride.insv", 20000, 30.0);
    const int v = t.addTrack(Track::Kind::Video, "Text");
    title::Style style;
    // Longer than "Hi": with Sans Serif, padding can make a two-letter picture
    // square on some systems (padX is larger than padY relative to the glyphs).
    style.text = "Hello";
    const int id = t.addTitleClip(v, 1000, style);
    title::Style blank;
    blank.text = "  ";
    t.addTitleClip(v, 8000, blank);

    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
    CHECK(feed.size() == 1 && jobs.size() == 1);
    if (feed.size() != 1 || jobs.size() != 1)
        return;
    const InsetFeed::Clip &f = feed[0];
    CHECK(f.id == id && f.path.isEmpty() && f.still);
    const exportpipe::Still fp = f.still->at(0);
    CHECK(fp.rgba && fp.width > 0 && fp.height > 0);
    CHECK(fp.width > fp.height); // a single line of text is wider than tall
    CHECK(f.still->nextChange(0) == -1); // the same all along
    // Some of the picture is text, some clear.
    int opaque = 0, clear = 0;
    for (size_t i = 3; i < fp.rgba->size(); i += 4)
        (*fp.rgba)[i] == 255 ? ++opaque : (*fp.rgba)[i] == 0 ? ++clear : 0;
    CHECK(opaque > 0 && clear > 0);
    const exportpipe::InsetClipJob &j = jobs[0];
    exportpipe::Still jp;
    // The picture's height follows the text size: ~8% of 1080 for the font,
    // plus the line's leading and padding.
    const inset::Rect r = titleRect(j, 0, 1920, 1080, &jp);
    CHECK(j.still && near(j.aspect, double(jp.width) / jp.height));
    CHECK(r.h > 0.08 * 1080 && r.h < 0.08 * 1080 * 2.0);
    CHECK(near(r.x + r.w / 2, 960.0, 1.0) && near(r.y + r.h / 2, 540.0, 1.0));
    // Playback's picture is smaller or the same, placed the same.
    CHECK(near(double(fp.width) / fp.height, j.aspect, 0.05));
    CHECK(near(r.w / 1920, f.look.width * fp.widthPerSize, 0.01));

    // Left-aligned, its position is its left edge, in playback and the
    // export alike, and stays there as the text grows; a box's edge, when
    // it has one.
    const auto leftEdge = [&t, &privacy] {
        const std::vector<exportpipe::InsetClipJob> js =
            renderjobs::insetJobs(t, 1920, 1080, privacy);
        const std::vector<InsetFeed::Clip> fs = renderjobs::insetFeed(t, 1920, 1080, privacy);
        if (js.size() != 1 || fs.size() != 1)
            return -1.0;
        const inset::Rect r = titleRect(js[0], 0);
        // Playback places it the same (title::drawnLook, its own picture).
        const InsetFeed::Clip &ff = fs[0];
        const exportpipe::Still fp = ff.still->at(0);
        const inset::Look pl = title::drawnLook(ff.look, fp.widthPerSize, fp.anchorX);
        const inset::Rect pr = inset::place(pl, {}, double(fp.width) / fp.height, 1920, 1080);
        return near(pr.x, r.x, 2.0) ? r.x : -2.0;
    };
    style.align = title::Align::Left;
    t.setTrackClipTitle(id, style);
    inset::Look at = t.trackClipById(id)->look;
    at.centerX = 0.1f;
    t.setTrackClipInset(id, at, {});
    const titlerender::Picture pic = titlerender::render(style, 400);
    const double pad = pic.anchorX * pic.width; // the picture's margin left of the text
    const double x0 = leftEdge();
    CHECK(x0 > 0.0 && near(x0 + pad * (0.08 * 1080 / 400.0), 0.1 * 1920, 2.0));
    style.text = "Hi there, a longer line";
    t.setTrackClipTitle(id, style);
    CHECK(near(leftEdge(), x0, 1.0));
    // Right-aligned: its right edge.
    style.align = title::Align::Right;
    t.setTrackClipTitle(id, style);
    {
        const std::vector<exportpipe::InsetClipJob> js =
            renderjobs::insetJobs(t, 1920, 1080, privacy);
        const titlerender::Picture rp = titlerender::render(style, 400);
        const inset::Rect r = titleRect(js[0], 0);
        const double margin = (1.0 - rp.anchorX) * r.w;
        CHECK(near(r.x + r.w - margin, 0.1 * 1920, 2.0));
    }
    // A new alignment moves the title so its text stays where it was.
    const title::Style centred = [&] {
        title::Style c = style;
        c.align = title::Align::Centre;
        return c;
    }();
    const double before = [&] {
        const auto js = renderjobs::insetJobs(t, 1920, 1080, privacy);
        const inset::Rect r = titleRect(js[0], 0);
        return r.x + r.w / 2;
    }();
    t.setTrackClipTitle(id, centred, renderjobs::titleAnchorShift(style, centred, 1920, 1080));
    {
        const auto js = renderjobs::insetJobs(t, 1920, 1080, privacy);
        const inset::Rect r = titleRect(js[0], 0);
        CHECK(near(r.x + r.w / 2, before, 1.0));
    }
}

// A title's motion: playback and the export move it the same way, and
// playback says how far it's from where the clip puts it (a drag then
// reports the clip's position).
void testTitleMotion()
{
    Timeline t;
    t.appendClip("/c/ride.insv", 20000, 30.0);
    const int v = t.addTrack(Track::Kind::Video, "Text");
    title::Style style;
    style.text = "Moving";
    style.align = title::Align::Left;
    inset::Motion m;
    m.in = {inset::Motion::Kind::Slide, inset::Motion::Side::Bottom, 1000};
    m.out = {inset::Motion::Kind::Pop, inset::Motion::Side::Left, 500};
    const int id = t.addTitleClip(v, 2000, style, 4000, title::defaultLook(), m);
    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
    CHECK(feed.size() == 1 && jobs.size() == 1);
    if (feed.size() != 1 || jobs.size() != 1)
        return;
    CHECK(feed[0].motion == m && jobs[0].motion == m && near(feed[0].outAspect, 1920.0 / 1080));
    InsetFeed player;
    player.setClips(feed);
    const exportpipe::InsetClipJob &j = jobs[0];
    const TrackClip &c = *t.trackClipById(id);
    for (qint64 ms : {2000, 2300, 2999, 4000, 5700, 5999}) {
        const std::vector<EquirectViewportWindow::InsetDraw> drawn = player.frameFor(ms, false);
        CHECK(drawn.size() == 1);
        if (drawn.size() != 1)
            continue;
        const EquirectViewportWindow::InsetDraw &d = drawn[0];
        const qint64 local = ms - c.startMs;
        const exportpipe::Still jp = j.still->at(local);
        const inset::Look el = inset::moved(
            inset::anchored(j.lookAt(c.sourceAtLocalMs(local)), jp.widthPerSize, jp.anchorX),
            j.motion, local, j.spanMs(), j.aspect, 1920.0 / 1080);
        const inset::Rect er = inset::place(el, {}, j.aspect, 1920, 1080);
        const inset::Rect pr = inset::place(d.look, {}, d.aspect, 1920, 1080);
        CHECK(near(pr.x, er.x, 2.0) && near(pr.y, er.y, 2.0) && near(pr.w, er.w, 2.0));
        // Where the clip puts it, back from where it's drawn.
        CHECK(near(d.look.centerY - d.anchorDy, c.look.centerY, 1e-5));
        if (ms == 2000)
            CHECK(er.y >= 1080 - 1); // just below the frame
        if (ms == 4000)
            CHECK(near(el.centerY, c.look.centerY, 1e-6)); // at rest
    }
}

// The columns of a picture with any ink (alpha above `min`): first, last.
std::pair<int, int> inkColumns(const titlerender::Picture &p, int min = 16)
{
    int first = p.width, last = -1;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            if ((*p.rgba)[(size_t(y) * p.width + x) * 4 + 3] > min) {
                first = std::min(first, x);
                last = std::max(last, x);
            }
    return {first, last};
}

// Typing on: how many letters show when, the picture at each state, and the
// typed part lined up by the alignment.
void testTypeOn()
{
    // Letters as a reader counts them: an accent on its letter is one, a
    // line break and a space are one each.
    CHECK(titlerender::graphemeCount(QString::fromUtf8("He\xcc\x81\ny o")) == 6);
    CHECK(titlerender::graphemeCount(QString()) == 0);

    inset::Motion m;
    m.in = {inset::Motion::Kind::Type, inset::Motion::Side::Left, 1000};
    m.out = {inset::Motion::Kind::Type, inset::Motion::Side::Left, 500};
    // Over the In: ceil(q * n), all (-1) once typed; at rest all; over the
    // Out the reverse.
    CHECK(titlerender::visibleAt(10, m, 0, 4000) == 0);
    CHECK(titlerender::visibleAt(10, m, 1, 4000) == 1);
    CHECK(titlerender::visibleAt(10, m, 100, 4000) == 1);
    CHECK(titlerender::visibleAt(10, m, 101, 4000) == 2);
    CHECK(titlerender::visibleAt(10, m, 300, 4000) == 3); // not 4: 0.3 * 10 is 3
    CHECK(titlerender::visibleAt(10, m, 999, 4000) == -1);
    CHECK(titlerender::visibleAt(10, m, 2000, 4000) == -1);
    CHECK(titlerender::visibleAt(10, m, 3500, 4000) == -1);
    CHECK(titlerender::visibleAt(10, m, 3550, 4000) == 9);
    CHECK(titlerender::visibleAt(10, m, 3999, 4000) == 1);
    CHECK(titlerender::visibleAt(10, m, 4000, 4000) == 0);
    // Too short for both: each in its share, as the slides.
    CHECK(titlerender::visibleAt(10, m, 500, 750) == -1);
    CHECK(titlerender::visibleAt(10, m, 250, 750) == 5);
    // A slide doesn't type.
    inset::Motion slide;
    slide.in = {inset::Motion::Kind::Slide, inset::Motion::Side::Left, 1000};
    CHECK(titlerender::visibleAt(10, slide, 100, 4000) == -1);

    // The pictures: the same size, anchor and box all along; the typed part
    // lined up by the alignment; a line typed in full as in the full text.
    for (const title::Align align :
         {title::Align::Left, title::Align::Centre, title::Align::Right}) {
        title::Style style;
        style.text = "Typing on\nslowly";
        style.align = align;
        style.box.on = true;
        style.outline.on = true;
        const titlerender::Picture full = titlerender::render(style, 60);
        const titlerender::Picture none = titlerender::render(style, 60, 0);
        const titlerender::Picture some = titlerender::render(style, 60, 3); // "Typ"
        const titlerender::Picture line2 = titlerender::render(style, 60, 13); // + "\nslo"
        const titlerender::Picture all = titlerender::render(style, 60, 16);
        CHECK(all.rgba == full.rgba); // every letter: the full picture
        for (const titlerender::Picture *p : {&none, &some, &line2}) {
            CHECK(p->width == full.width && p->height == full.height &&
                  p->widthPerFontPx == full.widthPerFontPx && p->anchorX == full.anchorX);
            CHECK(p->rgba != full.rgba && *p->rgba != *full.rgba);
            // The box is there from the start: its corner as in the full one.
            const size_t corner = (size_t(full.height / 2) * full.width + 3) * 4 + 3;
            CHECK((*p->rgba)[corner] == (*full.rgba)[corner]);
        }
        // Without a box: nothing at all before the first letter.
        title::Style bare = style;
        bare.box.on = false;
        const titlerender::Picture blank = titlerender::render(bare, 60, 0);
        CHECK(inkColumns(blank).second < 0);
        // The typed part of a line sits where the alignment says: from the
        // text's left edge, round its middle, or up to its right edge.
        const titlerender::Picture part = titlerender::render(bare, 60, 3);
        const titlerender::Picture whole = titlerender::render(bare, 60);
        const auto [pf, pl] = inkColumns(part);
        const auto [wf, wl] = inkColumns(whole);
        CHECK(pf > wf - 1 && pl < wl + 1 && pl - pf < (wl - wf) / 2);
        if (align == title::Align::Left)
            CHECK(std::abs(pf - wf) <= 1);
        else if (align == title::Align::Right)
            CHECK(std::abs(pl - wl) <= 6); // the last letter's side bearing
        else
            CHECK(std::abs((pf + pl) / 2.0 - part.width / 2.0) <= 6);
        // The second line under way: the first drawn whole, as in the full
        // picture -- the top rows match.
        const titlerender::Picture l2 = titlerender::render(bare, 60, 13);
        int differ = 0;
        for (int y = 0; y < l2.height / 3; ++y)
            for (int x = 0; x < l2.width; ++x)
                differ += (*l2.rgba)[(size_t(y) * l2.width + x) * 4 + 3] !=
                          (*whole.rgba)[(size_t(y) * l2.width + x) * 4 + 3];
        CHECK(differ == 0);
    }
    // Left-aligned, the typed letters are where the full line has them: no
    // reshaping as more come.
    {
        title::Style style;
        style.text = "AVATAR Wave";
        style.align = title::Align::Left;
        const titlerender::Picture whole = titlerender::render(style, 60);
        const titlerender::Picture part = titlerender::render(style, 60, 4); // "AVAT"
        const int split = inkColumns(titlerender::render(style, 60, 3)).second - 2;
        int differ = 0;
        for (int y = 0; y < whole.height; ++y)
            for (int x = 0; x < split; ++x)
                differ += std::abs((*part.rgba)[(size_t(y) * whole.width + x) * 4 + 3] -
                                   (*whole.rgba)[(size_t(y) * whole.width + x) * 4 + 3]) > 1;
        CHECK(differ == 0);
    }

    // TitleSource: its state and when it next changes, against every ms.
    title::Style style;
    style.text = "Typed caption";
    const int n = titlerender::graphemeCount(style.text);
    TitleSource src(style, m, 3000, 40, 1920, 1080);
    for (qint64 t = 0; t <= 3000; t += 7) {
        const int now = src.stateAt(t);
        CHECK(now == titlerender::visibleAt(n, m, t, 3000));
        qint64 expect = -1;
        for (qint64 u = t + 1; u <= 3000; ++u)
            if (src.stateAt(u) != now) {
                expect = u;
                break;
            }
        CHECK(src.nextChange(t) == expect);
    }
    // A title that doesn't type: one picture, today's.
    TitleSource still(style, slide, 3000, 40, 1920, 1080);
    CHECK(still.nextChange(0) == -1 && still.stateAt(500) == -1);
    CHECK(still.at(0).rgba == titlerender::render(style, 40).rgba);
    // Same order as TitleSource: widthScale = outH/outW, then *= widthPerFontPx.
    CHECK(still.at(0).widthPerSize ==
          titlerender::render(style, 40).widthPerFontPx * (1080.0 / 1920.0));
}

// Playback and the export show the same typed picture at the same moment,
// placed the same: playback once its decoder thread has made it.
void testTypeOnFeedVsExport()
{
    Timeline t;
    t.appendClip("/c/ride.insv", 20000, 30.0);
    const int v = t.addTrack(Track::Kind::Video, "Text");
    title::Style style;
    style.text = "Typed on, and off";
    style.align = title::Align::Right;
    style.shadow.on = true;
    inset::Motion m;
    m.in = {inset::Motion::Kind::Type, inset::Motion::Side::Left, 1500};
    m.out = {inset::Motion::Kind::Type, inset::Motion::Side::Left, 600};
    inset::Look look = title::defaultLook();
    look.centerX = 0.9f;
    const int id = t.addTitleClip(v, 2000, style, 4000, look, m);
    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
    CHECK(feed.size() == 1 && jobs.size() == 1);
    if (feed.size() != 1 || jobs.size() != 1)
        return;
    InsetFeed player;
    player.setClips(feed);
    const exportpipe::InsetClipJob &j = jobs[0];
    const TrackClip &c = *t.trackClipById(id);
    const auto shown = [&](qint64 ms, const exportpipe::Still &want) {
        // Ready within a while (the decoder thread renders it).
        for (int i = 0; i < 400; ++i) {
            const auto drawn = player.frameFor(ms, false);
            if (drawn.size() == 1 && drawn[0].rgba && *drawn[0].rgba == *want.rgba)
                return drawn;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::vector<EquirectViewportWindow::InsetDraw>();
    };
    std::set<const void *> pictures;
    for (qint64 ms : {2000, 2010, 2500, 3000, 3499, 3600, 5000, 5450, 5700, 5999}) {
        const qint64 local = ms - c.startMs;
        exportpipe::Still jp;
        const inset::Rect er = titleRect(j, local, 1920, 1080, &jp);
        CHECK(!jp.empty());
        if (jp.empty())
            continue;
        pictures.insert(jp.rgba.get());
        const auto drawn = shown(ms, jp);
        CHECK(drawn.size() == 1);
        if (drawn.size() != 1)
            continue;
        const inset::Rect pr = inset::place(drawn[0].look, {}, drawn[0].aspect, 1920, 1080);
        CHECK(near(pr.x, er.x, 1.0) && near(pr.y, er.y, 1.0) && near(pr.w, er.w, 1.0));
    }
    // A picture per state: 0, 1, 6 and 12 letters typed on, all of them, 16
    // and 9 typed off -- and 1 again (the same picture as before).
    CHECK(pictures.size() == 7);
}

// The clock's fields: their values per locale, without a recording time,
// escaped braces.
void testClockFields()
{
    const QDateTime at(QDate(2025, 8, 16), QTime(14, 5, 9), QTimeZone::UTC);
    const QLocale gb(QLocale::English, QLocale::UnitedKingdom);
    const QLocale us(QLocale::English, QLocale::UnitedStates);
    const QLocale de(QLocale::German, QLocale::Germany);
    CHECK(title::withFields("{time}", at, gb) == "14:05");
    CHECK(title::withFields("{time:ss}", at, gb) == "14:05:09");
    CHECK(title::withFields("{time:ss}", at, de) == "14:05:09");
    CHECK(title::withFields("{date}", at, gb) == gb.toString(at.date(), QLocale::LongFormat));
    // 12-hour where the locale writes it so.
    const QString usTime = title::withFields("{time}", at, us);
    const QString usSeconds = title::withFields("{time:ss}", at, us);
    CHECK(usTime == us.toString(at.time(), QLocale::ShortFormat));
    CHECK(usTime.startsWith("2:05") && usTime.contains("PM"));
    CHECK(usSeconds.startsWith("2:05:09") && usSeconds.contains("PM"));
    CHECK(title::withFields("At {time} on {date}.", at, gb) ==
          "At 14:05 on " + gb.toString(at.date(), QLocale::LongFormat) + ".");
    // Nothing says when: placeholders, no date.
    CHECK(title::withFields("{time}|{time:ss}|{date}", QDateTime(), gb) == "--:--|--:--:--|");
    // Braces that aren't fields.
    CHECK(title::withFields("{{time} {x} {time", at, gb) == "{time} {x} {time");
    CHECK(title::withFields("}{{{time}", at, gb) == "}{14:05");
    CHECK(title::hasFields("a {date}") && title::hasFields("{{{time}"));
    CHECK(!title::hasFields("{{time}") && !title::hasFields("{x}") && !title::hasFields("plain"));
    // The hours chosen rather than the system's.
    using title::Hours;
    CHECK(title::withFields("{time}", at, us, Hours::H24) == "14:05");
    CHECK(title::withFields("{time:ss}", at, us, Hours::H24) == "14:05:09");
    CHECK(title::withFields("{time}", at, us, Hours::H12) == usTime);
    CHECK(title::withFields("{time}", at, gb, Hours::H24) == "14:05");
    const QString gb12 = title::withFields("{time:ss}", at, gb, Hours::H12);
    CHECK(gb12.startsWith("2:05:09 ") && gb12.endsWith(gb.pmText()));
    const QString de12 = title::withFields("{time}", at, de, Hours::H12);
    CHECK(de12.startsWith("2:05 ") && de12.endsWith(de.pmText()));
    const QDateTime morning(QDate(2025, 8, 16), QTime(9, 5, 0), QTimeZone::UTC);
    CHECK(title::withFields("{time}", morning, us, Hours::H24) == "09:05");
    // A quoted literal in the locale's format is kept (Canadian French).
    const QLocale ca(QLocale::French, QLocale::Canada);
    CHECK(title::withFields("{time}", at, ca, Hours::H24) ==
          ca.toString(at.time(), QLocale::ShortFormat));
    CHECK(title::hasTimeField("{time:ss}") && !title::hasTimeField("{date} {{time}"));
    const QString old = "Price {time} {{";
    CHECK(title::withFields(title::escapeBraces(old), at, gb) == old);
}

// Which file plays where, and how far into it: trims, speed, cuts.
void testSourceTimeMap()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0);
    const int b = t.appendClip("/c/b.insv", 20000, 30.0);
    t.appendClip("/c/c.mp4", 20000, 30.0, 1920, 1080, 0, true);
    t.setClipTrimIn(a, 2000);
    t.setClipTrimOut(a, 10000); // 0..8 s
    t.setClipTrimIn(b, 3000);
    t.setClipTrimOut(b, 13000);
    t.setClipSpeed(b, 2.0); // 8..13 s; c 13..33 s
    const SourceTimeMap map = renderjobs::sourceTimeMap(t);
    CHECK(map.spans().size() == 3);
    CHECK(map.spanAt(0) == 0 && map.spanAt(7999) == 0);
    CHECK(map.spanAt(8000) == 1); // at the cut the later clip
    CHECK(map.spanAt(13000) == 2 && map.spanAt(99999) == 2);
    CHECK(map.spans()[1].path == "/c/b.insv");
    CHECK(map.sourceMs(0, 0) == 2000 && map.sourceMs(0, 7999) == 9999);
    CHECK(map.sourceMs(1, 8000) == 3000 && map.sourceMs(1, 9000) == 5000); // double speed
    CHECK(map.sourceMs(1, 20000) == 13000); // clamped to the span
    CHECK(map.sourceMs(2, 13000) == 0);
    CHECK(map.timelineMsAt(1, 8000, 5000) == 9000);
    CHECK(map.timelineMsAt(1, 8000, 5001) == 9001); // 5002 there: the first that reaches it
    CHECK(map.timelineMsAt(1, 9500, 5000) == 9500);
    CHECK(map.timelineMsAt(1, 8000, 13000) == -1); // only at the span's end
    CHECK(SourceTimeMap().spanAt(0) == -1);
}

// The clock over the main track: trims, speed, cuts, a clip without a
// recording time; when it ticks.
void testClock()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0);
    const int b = t.appendClip("/c/b.insv", 20000, 30.0);
    t.appendClip("/c/c.mp4", 20000, 30.0, 1920, 1080, 0, true);
    t.setClipTrimIn(a, 2000);
    t.setClipTrimOut(a, 10000); // 0..8 s
    t.setClipTrimIn(b, 3000);
    t.setClipTrimOut(b, 13000);
    t.setClipSpeed(b, 2.0); // 8..13 s; c from 13 s
    renderjobs::FieldSources rec;
    rec.recorded.insert("/c/a.insv", QDateTime(QDate(2025, 8, 16), QTime(10, 0, 0)));
    rec.recorded.insert("/c/b.insv", QDateTime(QDate(2025, 8, 16), QTime(23, 59, 50)));
    const title::Clock clock = renderjobs::titleClock(t, rec);
    const auto time = [&](qint64 ms) { return clock.at(ms).time(); };
    CHECK(time(0) == QTime(10, 0, 2)); // the trim-in's source time
    CHECK(time(999) == QTime(10, 0, 2) && time(1000) == QTime(10, 0, 3));
    CHECK(time(7999) == QTime(10, 0, 9));
    CHECK(time(8000) == QTime(23, 59, 53)); // the cut: the later clip
    CHECK(time(9000) == QTime(23, 59, 55)); // at double speed
    CHECK(clock.at(12999).date() == QDate(2025, 8, 17) && time(12999) == QTime(0, 0, 2));
    CHECK(!clock.at(13000).isValid() && !clock.at(40000).isValid());
    CHECK(clock.nextTick(0, 1) == 1000);
    CHECK(clock.nextTick(7500, 1) == 8000); // the next clip
    CHECK(clock.nextTick(8000, 1) == 8500);
    CHECK(clock.nextTick(0, 60) == 8000);
    CHECK(clock.nextTick(8000, 60) == 11500); // midnight
    CHECK(clock.nextTick(12999, 1) == 13000);
    CHECK(clock.nextTick(13000, 1) == -1);
    // Against every ms.
    for (qint64 ms = 0; ms < 13500; ms += 37) {
        const QDateTime now = clock.at(ms);
        qint64 expect = -1;
        for (qint64 u = ms + 1; u <= 13500; ++u)
            if (clock.at(u) != now || u == 8000 || u == 13000) {
                expect = u;
                break;
            }
        CHECK(clock.nextTick(ms, 1) == expect);
    }

    // A title's text over it, typing on, against every few ms: its state
    // and when the picture next changes.
    title::Style style;
    style.text = "{time:ss}";
    inset::Motion m;
    m.in = {inset::Motion::Kind::Type, inset::Motion::Side::Left, 1500};
    TitleSource src(style, m, 3000, 40, 1920, 1080, clock, 6500);
    CHECK(src.textAt(0) == title::withFields(style.text, clock.at(6500)));
    CHECK(clock.at(6500).time() == QTime(10, 0, 8));
    CHECK(src.textAt(1500) != src.textAt(0)); // the cut at 8 s
    const auto state = [&](qint64 ms) { return std::pair(src.textAt(ms), src.stateAt(ms)); };
    for (qint64 ms = 0; ms <= 3000; ms += 13) {
        const auto now = state(ms);
        qint64 expect = -1;
        for (qint64 u = ms + 1; u <= 3000; ++u)
            if (state(u) != now) {
                expect = u;
                break;
            }
        CHECK(src.nextChange(ms) == expect);
    }
    // Without fields the clock is never read: the text as it was, its one
    // picture.
    title::Style plain;
    plain.text = "Plain {x}";
    TitleSource fixed(plain, {}, 3000, 40, 1920, 1080, clock, 6500);
    CHECK(fixed.nextChange(0) == -1 && fixed.textAt(1500) == "Plain {x}");
    CHECK(fixed.at(0).rgba == titlerender::render(plain, 40).rgba);
}

// Playback and the export show the same clock at the same moment, across
// a second and a cut; a date with no recording time shows nothing.
void testClockFeedVsExport()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0);
    t.appendClip("/c/b.insv", 20000, 30.0);
    t.setClipTrimOut(a, 3000);
    const int v = t.addTrack(Track::Kind::Video, "Clock");
    title::Style style;
    style.text = "{time:ss}";
    style.align = title::Align::Right;
    inset::Look look = title::defaultLook();
    look.centerX = 0.95f;
    const int id = t.addTitleClip(v, 1000, style, 4000, look);
    const int none = t.addTitleClip(t.addTrack(Track::Kind::Video, "Date"), 0, [] {
        title::Style s;
        s.text = "{date}";
        return s;
    }(), 2000);
    renderjobs::FieldSources rec;
    rec.recorded.insert("/c/a.insv", QDateTime(QDate(2025, 8, 16), QTime(10, 0, 0)));
    const PrivacyBlur privacy;
    // No recording time: kept as a layer, nothing drawn.
    {
        const auto jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
        CHECK(jobs.size() == 2);
        for (const auto &j : jobs)
            CHECK(j.still && j.still->at(500).empty() == (j.startMs == 0));
    }
    t.removeTrackClip(none);
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy, rec);
    const std::vector<exportpipe::InsetClipJob> jobs =
        renderjobs::insetJobs(t, 1920, 1080, privacy, rec);
    CHECK(feed.size() == 1 && jobs.size() == 1);
    if (feed.size() != 1 || jobs.size() != 1)
        return;
    InsetFeed player;
    player.setClips(feed);
    const exportpipe::InsetClipJob &j = jobs[0];
    const TrackClip &c = *t.trackClipById(id);
    std::set<const void *> pictures;
    // 10:00:01, :01, :02; the cut at 3 s: b has no time -- "--:--:--".
    for (qint64 ms : {1000, 1999, 2000, 2999, 3000, 4999}) {
        exportpipe::Still jp;
        const inset::Rect er = titleRect(j, ms - c.startMs, 1920, 1080, &jp);
        CHECK(!jp.empty());
        if (jp.empty())
            continue;
        pictures.insert(jp.rgba.get());
        std::vector<EquirectViewportWindow::InsetDraw> drawn;
        for (int i = 0; i < 400; ++i) {
            drawn = player.frameFor(ms, false);
            if (drawn.size() == 1 && drawn[0].rgba && *drawn[0].rgba == *jp.rgba)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(drawn.size() == 1 && drawn[0].rgba && *drawn[0].rgba == *jp.rgba);
        if (drawn.size() != 1)
            continue;
        const inset::Rect pr = inset::place(drawn[0].look, {}, drawn[0].aspect, 1920, 1080);
        // Right-aligned: its right edge stays put as the digits change.
        CHECK(near(pr.x + pr.w, er.x + er.w, 1.0) && near(pr.y, er.y, 1.0) &&
              near(pr.w, er.w, 1.0));
    }
    CHECK(pictures.size() == 3);
}

// The GPS fields: whole values in the title's units, a stop as 0, the
// heading round to north, `--` with no data.
void testGpsFields()
{
    const QLocale gb(QLocale::English, QLocale::UnitedKingdom);
    const QLocale de(QLocale::German, QLocale::Germany);
    Telemetry::Values v;
    v.speedMps = 20.0;
    v.altitudeM = 120.0;
    v.headingDeg = 45.0;
    const title::Units metric;
    const title::Units imperial{title::SpeedUnit::Mph, title::AltitudeUnit::Feet};
    const auto fields = [&](const QString &text, const std::optional<Telemetry::Values> &gps,
                            title::Units u = {}, const QLocale &l = QLocale(QLocale::C)) {
        return title::withFields(text, QDateTime(), l, title::Hours::System, gps, u);
    };
    CHECK(fields("{speed}", v, metric, gb) == "72 km/h");
    CHECK(fields("{speed}", v, imperial, gb) == "45 mph"); // 44.7
    CHECK(fields("{altitude}", v, metric, gb) == "120 m");
    CHECK(fields("{altitude}", v, imperial, gb) == "394 ft"); // 393.7
    CHECK(fields("{heading}|{heading:compass}", v, metric, gb) == QString("45\u00b0|NE"));
    v.altitudeM = 1234.4;
    CHECK(fields("{altitude}", v, metric, de) == "1.234 m"); // as the locale writes numbers
    v.speedMps = 0.4; // GPS jitter standing still
    v.headingDeg = 359.6;
    CHECK(fields("{speed} {heading} {heading:compass}", v) == QString("0 km/h 0\u00b0 N"));
    v.speedMps = 0.5;
    CHECK(fields("{speed}", v) == "2 km/h");
    v.headingDeg = 202.0;
    CHECK(fields("{heading:compass}", v) == "S");
    v.headingDeg = 203.0;
    CHECK(fields("{heading:compass}", v) == "SW");
    v.headingDeg.reset(); // never moved in this stretch
    CHECK(fields("{heading}|{heading:compass}|{speed}", v) == QString("--\u00b0|--|2 km/h"));
    CHECK(fields("{speed}|{altitude}|{heading:compass}", std::nullopt, imperial) ==
          "-- mph|-- ft|--");
    CHECK(fields("{{speed} {speedx}", v) == "{speed} {speedx}");
    CHECK(fields("At {speed}, {time}", v).startsWith("At 2 km/h, --:--"));
    CHECK(title::hasGpsFields("{heading:compass}") && !title::hasGpsFields("{time} {{speed}"));
    CHECK(title::hasClockFields("{date} {speed}") && !title::hasClockFields("{speed}"));
    CHECK(title::hasFields("{altitude}"));
    CHECK(title::defaultUnits(QLocale(QLocale::English, QLocale::UnitedStates)) == imperial);
    CHECK(title::defaultUnits(de) == metric);
}

// A clip's GPS: telemetry 10 m/s at its start, a metre per second faster
// each second -- a speed every 3.6 km/h, so the shown value changes every
// ~280 ms.
std::shared_ptr<const Telemetry> rampTelemetry()
{
    insv::GpsTrack track;
    for (int s = 0; s <= 20; ++s)
        track.fixes.push_back({s * 1000, 59.0, 18.0, 10.0 + s, 90.0, 100.0 + s});
    return std::make_shared<const Telemetry>(track);
}

// The GPS read along the timeline: trims, speed, cuts; a clip without GPS
// shows none. A title's text over it, and when it next changes.
void testGpsClock()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0);
    const int b = t.appendClip("/c/b.insv", 20000, 30.0);
    t.setClipTrimIn(a, 2000);
    t.setClipTrimOut(a, 10000); // 0..8 s
    t.setClipSpeed(b, 2.0);     // 8..18 s
    renderjobs::FieldSources sources;
    sources.gps.insert("/c/a.insv", rampTelemetry());
    const title::Clock clock = renderjobs::titleClock(t, sources);
    CHECK(clock.gpsAt(0) && near(clock.gpsAt(0)->speedMps, 12.0, 1e-9)); // source 2 s
    CHECK(clock.gpsAt(7500) && near(clock.gpsAt(7500)->altitudeM, 109.5, 1e-9));
    CHECK(!clock.gpsAt(8000) && !clock.gpsAt(30000)); // b: no GPS
    CHECK(!clock.at(0).isValid());                     // and no recording time

    title::Style style;
    style.text = "{speed}";
    TitleSource src(style, {}, 9000, 40, 1920, 1080, clock, 0);
    CHECK(src.textAt(0) == title::withFields("{speed}", QDateTime(), QLocale(),
                                             title::Hours::System, Telemetry::Values{0, 0, 12.0}));
    CHECK(src.textAt(8500).startsWith("--"));
    // Against every ms, as far ahead as it looks.
    for (qint64 ms = 0; ms < 9000; ms += 41) {
        const QString now = src.textAt(ms);
        qint64 expect = -1;
        for (qint64 u = ms + 1; u <= std::min<qint64>(9000, ms + TitleSource::kGpsLookaheadMs);
             ++u)
            if (src.textAt(u) != now) {
                expect = u;
                break;
            }
        CHECK(src.nextChange(ms) == expect);
    }
}

// A clip's motion: leaning 1 degree further right each second, from upright
// at its start, the G-forces likewise in hundredths of a g.
insv::MotionTrack rampMotion(int seconds)
{
    insv::MotionTrack m;
    for (int i = 0; i <= seconds * 100; ++i) {
        m.leanDeg.push_back(float(i) / 100.0f);
        m.lateralG.push_back(float(i) / 10000.0f);
        m.forwardG.push_back(-float(i) / 10000.0f);
    }
    return m;
}

// The lean and G-forces read along the timeline like the GPS: trims, speed,
// cuts; a clip without them has none.
void testMotionClock()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0);
    const int b = t.appendClip("/c/b.insv", 20000, 30.0);
    t.appendClip("/c/c.insv", 20000, 30.0);
    t.setClipTrimIn(a, 2000);
    t.setClipTrimOut(a, 10000); // 0..8 s
    t.setClipSpeed(b, 2.0);     // 8..18 s
    renderjobs::FieldSources sources;
    sources.motion.insert("/c/a.insv", std::make_shared<const MotionTelemetry>(rampMotion(20)));
    sources.motion.insert("/c/b.insv", std::make_shared<const MotionTelemetry>(rampMotion(20)));
    const title::Clock clock = renderjobs::titleClock(t, sources);
    const auto at = [&](qint64 ms) { return clock.motionAt(ms); };
    CHECK(at(0) && near(at(0)->leanDeg, 2.0, 1e-4)); // source 2 s
    CHECK(at(7500) && near(at(7500)->lateralG, 0.095, 1e-6) &&
          near(at(7500)->forwardG, -0.095, 1e-6));
    CHECK(at(8000) && near(at(8000)->leanDeg, 0.0, 1e-4));  // b from its start ...
    CHECK(at(13000) && near(at(13000)->leanDeg, 10.0, 1e-4)); // ... at twice the speed
    CHECK(!at(18000) && !at(30000)); // c: none
    CHECK(!clock.gpsAt(0));          // and no GPS
}

// Each file's GPS and motion read once in the background; nothing until
// it's in, then ready(); await() waits for it; a forgotten file is read
// again, and the read it replaced isn't announced.
void testTelemetryCache()
{
    std::atomic<int> reads{0};
    std::mutex gate; // held: a read waits for it
    const auto read = [&](const std::string &path) {
        ++reads;
        std::lock_guard<std::mutex> wait(gate);
        camera::ClipTelemetry t;
        if (path == "/c/ride.insv") {
            t.gps = insv::GpsTrack{{{0, 59.0, 18.0, 10.0, 90.0, 5.0},
                                    {1000, 59.0, 18.0, 11.0, 90.0, 5.0}}};
            t.motion = rampMotion(2);
        }
        return t;
    };
    TelemetryCache cache(read);
    QStringList announced;
    QObject::connect(&cache, &TelemetryCache::ready, [&](const QString &p) { announced << p; });
    // ready() is queued from the read's thread.
    const auto announcements = [&] {
        for (int i = 0; i < 200 && announced.isEmpty(); ++i) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        QCoreApplication::processEvents();
        return std::exchange(announced, {});
    };

    gate.lock();
    const TelemetryCache::Readings first = cache.readings("/c/ride.insv");
    CHECK(!first.gps && !first.motion && cache.pending("/c/ride.insv"));
    gate.unlock();
    const TelemetryCache::Readings waited = cache.await("/c/ride.insv");
    CHECK(waited.gps && waited.motion && !cache.pending("/c/ride.insv"));
    CHECK(waited.motion && waited.motion->at(1000) &&
          near(waited.motion->at(1000)->leanDeg, 1.0, 1e-4));
    CHECK(announcements() == QStringList{"/c/ride.insv"});
    CHECK(cache.readings("/c/ride.insv").gps == waited.gps); // the same, not read again
    CHECK(reads == 1);

    // A file without either: none, and not read again either.
    const TelemetryCache::Readings none = cache.await("/c/plain.mp4");
    CHECK(!none.gps && !none.motion);
    cache.readings("/c/plain.mp4");
    CHECK(reads == 2);
    CHECK(announcements() == QStringList{"/c/plain.mp4"});

    // Forgotten while it's read: read again, only the new read announced.
    cache.forget({"/c/ride.insv"});
    gate.lock();
    CHECK(!cache.readings("/c/ride.insv").gps);
    cache.forget({"/c/ride.insv"});
    CHECK(!cache.pending("/c/ride.insv"));
    gate.unlock();
    const TelemetryCache::Readings again = cache.await("/c/ride.insv");
    CHECK(again.gps && again.gps != waited.gps && reads == 4);
    CHECK(announcements() == QStringList{"/c/ride.insv"});
}

// Playback and the export show the same speed at the same moment, across
// value changes and into a clip without GPS.
void testGpsFeedVsExport()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0);
    t.appendClip("/c/b.insv", 20000, 30.0);
    t.setClipTrimOut(a, 3000);
    const int v = t.addTrack(Track::Kind::Video, "Speed");
    title::Style style;
    style.text = "{speed}";
    style.align = title::Align::Left;
    inset::Look look = title::defaultLook();
    look.centerX = 0.05f;
    const int id = t.addTitleClip(v, 1000, style, 4000, look);
    renderjobs::FieldSources sources;
    sources.gps.insert("/c/a.insv", rampTelemetry());
    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy, sources);
    const std::vector<exportpipe::InsetClipJob> jobs =
        renderjobs::insetJobs(t, 1920, 1080, privacy, sources);
    CHECK(feed.size() == 1 && jobs.size() == 1);
    if (feed.size() != 1 || jobs.size() != 1)
        return;
    InsetFeed player;
    player.setClips(feed);
    const exportpipe::InsetClipJob &j = jobs[0];
    const TrackClip &c = *t.trackClipById(id);
    std::set<std::vector<uint8_t>> pictures;
    // 40, 41 and 43 km/h; the cut at 3 s: b has no GPS -- "-- km/h".
    for (qint64 ms : {1000, 1300, 1999, 3000, 4999}) {
        exportpipe::Still jp;
        const inset::Rect er = titleRect(j, ms - c.startMs, 1920, 1080, &jp);
        CHECK(!jp.empty());
        if (jp.empty())
            continue;
        pictures.insert(*jp.rgba);
        std::vector<EquirectViewportWindow::InsetDraw> drawn;
        for (int i = 0; i < 400; ++i) {
            drawn = player.frameFor(ms, false);
            if (drawn.size() == 1 && drawn[0].rgba && *drawn[0].rgba == *jp.rgba)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(drawn.size() == 1 && drawn[0].rgba && *drawn[0].rgba == *jp.rgba);
        if (drawn.size() != 1)
            continue;
        const inset::Rect pr = inset::place(drawn[0].look, {}, drawn[0].aspect, 1920, 1080);
        // Left-aligned: its left edge stays put as the digits change.
        CHECK(near(pr.x, er.x, 1.0) && near(pr.y, er.y, 1.0) && near(pr.w, er.w, 1.0));
    }
    CHECK(pictures.size() == 4); // 3000 and 4999 both "-- km/h"
}

// A gauge's reading: its number as a title's field writes it, its
// position clamped to the scale and rounded only as finely as it shows; a
// tape the heading; none without GPS. Its pictures: its kind's shape, one
// per reading, kept.
void testGaugeReadings()
{
    Telemetry::Values v;
    v.speedMps = 20.0; // 72 km/h, 44.7 mph
    v.altitudeM = 1234.4;
    v.headingDeg = 359.6;
    gauge::Style dial;
    const gaugerender::Reading r = gaugerender::readingOf(dial, v, 200);
    CHECK(r.hasValue && r.number == 72 && near(r.position, 72.0, 0.5));
    dial.units.speed = title::SpeedUnit::Mph;
    CHECK(gaugerender::readingOf(dial, v, 200).number == 45);
    dial.max = 40.0; // past the end: the needle there, the number as it is
    const gaugerender::Reading over = gaugerender::readingOf(dial, v, 200);
    CHECK(over.number == 45 && near(over.position, 40.0, 0.06)); // to the half pixel
    v.speedMps = 0.4; // standing still
    CHECK(gaugerender::readingOf(dial, v, 200).number == 0);
    gauge::Style bar;
    bar.kind = gauge::Kind::Bar;
    bar.reads = gauge::Reads::Altitude;
    bar.max = 2000.0;
    CHECK(gaugerender::readingOf(bar, v, 100).number == 1234);
    // Finer on a bigger picture.
    v.altitudeM = 1234.37;
    const double small = gaugerender::readingOf(bar, v, 50).position;
    const double big = gaugerender::readingOf(bar, v, 1000).position;
    CHECK(std::abs(big - 1234.37) < std::abs(small - 1234.37) + 1e-9 &&
          std::abs(small - 1234.37) <= 2000.0 / (2.84 * 50) + 1e-9);
    gauge::Style tape;
    tape.kind = gauge::Kind::Tape;
    const gaugerender::Reading heading = gaugerender::readingOf(tape, v, 100);
    CHECK(heading.hasValue && heading.number == 0 && near(heading.position, 359.6, 0.2));
    v.headingDeg.reset();
    CHECK(!gaugerender::readingOf(tape, v, 100).hasValue);
    CHECK(!gaugerender::readingOf(bar, std::nullopt, 100).hasValue);

    for (const gauge::Style &s : {gauge::Style{}, bar, tape}) {
        const gaugerender::Picture p = gaugerender::render(s, 120, r);
        CHECK(!p.empty() && p.height == 120 &&
              p.width == int(std::lround(gauge::aspect(s.kind) * 120)));
        CHECK(gaugerender::cached(s, 120, r).rgba == p.rgba); // kept
        // Something drawn, and the value shows.
        CHECK(*gaugerender::render(s, 120, gaugerender::Reading{}).rgba != *p.rgba);
    }
}

// A gauge's next change against every ms, as far ahead as it looks.
void testGaugeSource()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0);
    t.appendClip("/c/b.insv", 20000, 30.0);
    t.setClipTrimOut(a, 3000);
    renderjobs::FieldSources sources;
    sources.gps.insert("/c/a.insv", rampTelemetry());
    const title::Clock clock = renderjobs::titleClock(t, sources);
    gauge::Style style;
    style.kind = gauge::Kind::Bar;
    GaugeSource src(style, 5000, 60, 1920, 1080, clock, 0);
    CHECK(src.readingAt(0).hasValue && src.readingAt(0).number == 36);
    CHECK(!src.readingAt(3500).hasValue); // b: no GPS
    const exportpipe::Still still = src.at(1000);
    CHECK(!still.empty() && still.height == 60 &&
          near(still.widthPerSize, gauge::aspect(style.kind) * 1080.0 / 1920.0, 1e-12));
    CHECK(src.ready(1000).rgba == still.rgba);
    for (qint64 ms = 0; ms < 5000; ms += 53) {
        const gaugerender::Reading now = src.readingAt(ms);
        qint64 expect = -1;
        for (qint64 u = ms + 1; u <= std::min<qint64>(5000, ms + GaugeSource::kLookaheadMs); ++u)
            if (src.readingAt(u) != now) {
                expect = u;
                break;
            }
        CHECK(src.nextChange(ms) == expect);
    }
}

// Playback and the export show a gauge alike: the same picture, the same
// place, across value changes and into a clip without GPS.
void testGaugeFeedVsExport()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0);
    t.appendClip("/c/b.insv", 20000, 30.0);
    t.setClipTrimOut(a, 3000);
    const int v = t.addTrack(Track::Kind::Video, "Gauge");
    const int id = t.addGaugeClip(v, 1000, gauge::Style{}, 4000);
    renderjobs::FieldSources sources;
    sources.gps.insert("/c/a.insv", rampTelemetry());
    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy, sources);
    const std::vector<exportpipe::InsetClipJob> jobs =
        renderjobs::insetJobs(t, 1920, 1080, privacy, sources);
    CHECK(feed.size() == 1 && jobs.size() == 1);
    if (feed.size() != 1 || jobs.size() != 1)
        return;
    CHECK(near(feed[0].aspect, 1.0, 1e-12) && near(jobs[0].aspect, 1.0, 1e-12));
    InsetFeed player;
    player.setClips(feed);
    const TrackClip &c = *t.trackClipById(id);
    std::set<std::vector<uint8_t>> pictures;
    for (qint64 ms : {1000, 1500, 2999, 3000, 4999}) {
        exportpipe::Still jp;
        const inset::Rect er = titleRect(jobs[0], ms - c.startMs, 1920, 1080, &jp);
        CHECK(!jp.empty());
        if (jp.empty())
            continue;
        pictures.insert(*jp.rgba);
        std::vector<EquirectViewportWindow::InsetDraw> drawn;
        for (int i = 0; i < 400; ++i) {
            drawn = player.frameFor(ms, false);
            if (drawn.size() == 1 && drawn[0].rgba && *drawn[0].rgba == *jp.rgba)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(drawn.size() == 1 && drawn[0].rgba && *drawn[0].rgba == *jp.rgba);
        if (drawn.size() != 1)
            continue;
        const inset::Rect pr = inset::place(drawn[0].look, {}, drawn[0].aspect, 1920, 1080);
        CHECK(near(pr.x, er.x, 1.0) && near(pr.y, er.y, 1.0) && near(pr.w, er.w, 1.0));
        // Its height the look's width, of the output height; centred on it.
        CHECK(near(er.h, gauge::defaultLook().width * 1080.0, 1.5));
        CHECK(near(er.x + er.w / 2.0, gauge::defaultLook().centerX * 1920.0, 1.0));
    }
    CHECK(pictures.size() == 4); // 3000 and 4999 both empty
}

// A transition: the other clip as a layer over each half, blending through
// the cut; the clips' own sound running on past it, crossfading.
void testTransitions()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0, 7680, 3840, 1);
    const int b = t.appendClip("/c/b.insv", 20000, 30.0, 7680, 3840, 1);
    const int c = t.appendClip("/c/c.mp4", 20000, 30.0, 1920, 1080, 0, true);
    t.setClipTrimIn(a, 2000);
    t.setClipTrimOut(a, 10000); // 8 s
    t.setClipTrimIn(b, 3000);
    t.setClipTrimOut(b, 13000);
    t.setClipSpeed(b, 2.0); // 5 s
    t.setClipTrimIn(c, 4000);
    t.setTransition(a, Transition::Type::Crossfade, 1000); // the cut at 8 s
    t.setTransition(b, Transition::Type::Crossfade, 2000); // the cut at 13 s

    const std::vector<renderjobs::TransitionLayer> layers = renderjobs::transitionLayers(t);
    CHECK(layers.size() == 4);
    if (layers.size() != 4)
        return;
    // a | b: b from before its trim-in (at double speed), blending in to
    // half; then a past its trim-out, blending out from half.
    const renderjobs::TransitionLayer &in = layers[0], &out = layers[1];
    CHECK(in.clipIndex == 1 && in.startMs == 7500 && in.trimInMs == 2000 && in.trimOutMs == 3000);
    CHECK(in.rampFrom == 0.0f && in.rampTo == 0.5f);
    CHECK(out.clipIndex == 0 && out.startMs == 8000 && out.trimInMs == 10000 &&
          out.trimOutMs == 10500);
    CHECK(out.rampFrom == 0.5f && out.rampTo == 0.0f);
    CHECK(in.id < 0 && out.id < 0 && in.id != out.id && layers[2].id != in.id);
    CHECK(in.overClipId == a && out.overClipId == b); // each over the other clip
    CHECK(layers[2].clipIndex == 2 && layers[2].startMs == 12000 && layers[3].clipIndex == 1);

    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
    // The flat clip's own layer, then the four transition layers.
    CHECK(feed.size() == 5 && jobs.size() == 5);
    if (feed.size() != 5 || jobs.size() != 5)
        return;
    CHECK(feed[0].id == c && !feed[0].picture);
    const InsetFeed::Clip &f = feed[1];
    CHECK(f.id == in.id && f.fullFrame && f.picture && !f.pictureKey.isEmpty());
    CHECK(f.startMs == 7500 && f.trimInMs == 2000 && f.speed == 2.0 && f.spanMs() == 500);
    CHECK(near(f.aspect, 1920.0 / 1080.0) && near(f.look.width, 1.0));
    CHECK(f.rampFrom == 0.0f && f.rampTo == 0.5f);
    CHECK(jobs[1].picture && jobs[1].rampTo == 0.5f && jobs[1].startMs == 7500);
    // b | c: the flat clip blends in as its file's frames.
    CHECK(feed[3].path == "/c/c.mp4" && !feed[3].picture && feed[3].trimInMs == 3000);
    // A new setting of the clip it shows: a new picture.
    const QString key = f.pictureKey;
    ColorGrade g;
    g.exposureEv = 0.5f;
    t.setClipColor(b, g);
    CHECK(renderjobs::insetFeed(t, 1920, 1080, privacy)[1].pictureKey != key);

    // The sound: a's runs half the transition past the cut and fades out
    // over all of it, b's starts half of it early and fades in.
    std::vector<insv::MixClip> mix = renderjobs::mixClips(t);
    const auto clipOf = [&](const char *path) -> const insv::MixClip * {
        for (const insv::MixClip &m : mix)
            if (m.path == path)
                return &m;
        return nullptr;
    };
    const insv::MixClip *ma = clipOf("/c/a.insv");
    const insv::MixClip *mb = clipOf("/c/b.insv");
    CHECK(ma && mb);
    if (!ma || !mb)
        return;
    CHECK(ma->startFrame == 0 && ma->lengthFrames == 8500 * 48 && ma->fadeOutFrames == 1000 * 48);
    CHECK(ma->fadeInFrames == 0);
    CHECK(mb->startFrame == 7500 * 48 && mb->inFrame == 2000 * 48 && mb->fadeInFrames == 1000 * 48);
    // ... and at its own end into c (a 2 s transition).
    CHECK(mb->lengthFrames == (500 + 5000 + 1000) * 48 && mb->fadeOutFrames == 2000 * 48);

    // One that can't play (no footage before c's start) has no layers.
    t.setClipTrimIn(c, 0);
    CHECK(renderjobs::transitionLayers(t).size() == 2);
    mix = renderjobs::mixClips(t);
    mb = clipOf("/c/b.insv");
    CHECK(mb && mb->fadeOutFrames == 0);

    // A dip to black (b | c still can't play): black over a | b, in up to
    // the cut and out after it; the sound fades out and in, no longer.
    t.setTransition(a, Transition::Type::DipToBlack, 1000);
    const std::vector<renderjobs::TransitionLayer> dip = renderjobs::transitionLayers(t);
    CHECK(dip.size() == 2);
    if (dip.size() != 2)
        return;
    CHECK(dip[0].clipIndex == -1 && dip[0].startMs == 7500 &&
          dip[0].trimOutMs - dip[0].trimInMs == 500);
    CHECK(dip[0].rampFrom == 0.0f && dip[0].rampTo == 1.0f);
    CHECK(dip[0].overClipId == a && dip[1].overClipId == b);
    CHECK(dip[1].clipIndex == -1 && dip[1].startMs == 8000 && dip[1].rampFrom == 1.0f &&
          dip[1].rampTo == 0.0f);
    const std::vector<InsetFeed::Clip> dipFeed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> dipJobs =
        renderjobs::insetJobs(t, 1920, 1080, privacy);
    CHECK(dipFeed.size() == 3 && dipJobs.size() == 3);
    if (dipFeed.size() != 3 || dipJobs.size() != 3)
        return;
    const InsetFeed::Clip &black = dipFeed[1];
    CHECK(black.id == dip[0].id && black.still && black.fullFrame && black.speed == 1.0);
    CHECK(black.spanMs() == 500 && black.rampTo == 1.0f && near(black.look.width, 1.0));
    const exportpipe::Still bp = black.still->at(0);
    CHECK(bp.width > 0 && bp.rgba->size() == size_t(bp.width * bp.height * 4));
    CHECK(bp.rgba->at(0) == 0 && bp.rgba->at(3) == 255); // opaque black
    CHECK(bp.widthPerSize == 0.0); // fills the layer
    CHECK(dipJobs[1].still == black.still && dipJobs[1].rampTo == 1.0f &&
          dipJobs[1].startMs == 7500);
    mix = renderjobs::mixClips(t);
    ma = clipOf("/c/a.insv");
    mb = clipOf("/c/b.insv");
    CHECK(ma && ma->lengthFrames == 8000 * 48 && ma->fadeOutFrames == 500 * 48);
    CHECK(mb && mb->startFrame == 8000 * 48 && mb->inFrame == 3000 * 48 &&
          mb->fadeInFrames == 500 * 48);
}

// Playback draws a transition's layer only over the main-track clip it
// blends with, and a little past its span: the main picture may lag the
// clock at the cut, and the layer goes with the picture, not the clock.
void testLayerOverMainClip()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0, 7680, 3840, 1);
    const int b = t.appendClip("/c/b.insv", 20000, 30.0, 7680, 3840, 1);
    t.setClipTrimOut(a, 8000);
    t.setTransition(a, Transition::Type::DipToBlack, 1000); // black 7.5-8 s over a, 8-8.5 s over b
    InsetFeed feed;
    feed.setClips(renderjobs::insetFeed(t, 1920, 1080, PrivacyBlur()));
    const auto drawn = [&](qint64 ms, int mainClipId) {
        return feed.frameFor(ms, false, mainClipId);
    };
    CHECK(drawn(7600, a).size() == 1);
    CHECK(drawn(7600, b).empty() && drawn(7600, 0).empty());
    // Just past the cut, the main track still showing a: still a's layer,
    // at its end (all black).
    const std::vector<EquirectViewportWindow::InsetDraw> late = drawn(8010, a);
    CHECK(late.size() == 1 && late[0].look.opacity > 0.99f && late[0].locked);
    // b on screen: the layer going out over it.
    const std::vector<EquirectViewportWindow::InsetDraw> over = drawn(8010, b);
    CHECK(over.size() == 1 && over[0].look.opacity > 0.9f);
    CHECK(over.size() == 1 && late.size() == 1 && over[0].clipId != late[0].clipId);
    // b showing a frame early: already b's layer, at its start.
    CHECK(drawn(7990, b).size() == 1);
    // Well outside: none.
    CHECK(drawn(7000, a).empty() && drawn(9000, b).empty());
}

void testMissingSources()
{
    Timeline t;
    t.appendClip("/c/here.insv", 20000, 30.0);
    t.appendClip("/c/gone.insv", 20000, 30.0);
    t.appendClip("/c/gone.insv", 20000, 30.0); // the same file twice: listed once
    const int music = t.addTrack(Track::Kind::Audio, "Music");
    const int mutedTrack = t.addTrack(Track::Kind::Audio, "Muted");
    t.setTrackAudio(mutedTrack, 0.0, true);
    t.addAudioClip(music, "/m/song.wav", 5000, 0);
    const int quiet = t.addAudioClip(music, "/m/quiet.wav", 5000, 6000);
    t.setAudioClipLevel(quiet, 0.0, true);
    t.addAudioClip(mutedTrack, "/m/off.wav", 5000, 0);
    const int v1 = t.addTrack(Track::Kind::Video, "V1");
    const int hidden = t.addTrack(Track::Kind::Video, "Hidden");
    t.setTrackAudio(hidden, 0.0, true);
    t.addVideoClip(v1, "/v/inset.mp4", 4000, 0, 1920, 1080);
    t.addVideoClip(hidden, "/v/hidden.mp4", 4000, 0, 1920, 1080);

    CHECK(renderjobs::missingSources(t, renderjobs::MissingFor::Timeline).isEmpty());

    t.setSourcesMissing({{"/c/gone.insv", true},
                         {"/m/song.wav", true},
                         {"/m/quiet.wav", true},
                         {"/m/off.wav", true},
                         {"/v/inset.mp4", true},
                         {"/v/hidden.mp4", true}});
    // Muted clip, muted track and hidden track aren't exported: not listed.
    const QList<renderjobs::MissingSource> missing =
        renderjobs::missingSources(t, renderjobs::MissingFor::Export);
    QStringList paths;
    for (const renderjobs::MissingSource &m : missing)
        paths << m.path;
    CHECK((paths == QStringList{"/c/gone.insv", "/m/song.wav", "/v/inset.mp4"}));
    if (missing.size() == 3)
        CHECK((missing[0].places == QStringList{"clip 2", "clip 3"}));
    // The display lines name each file's place on the timeline (and its
    // path under it), so several missing at once stay tellable apart.
    const QStringList lines = renderjobs::missingSourceLines(t, renderjobs::MissingFor::Export);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0].startsWith(QLatin1String("gone.insv -- clip 2, clip 3")));
        CHECK(lines[0].contains(QLatin1String("/c/gone.insv")));
        CHECK(lines[1].startsWith(QStringLiteral("song.wav -- sound on “Music”")));
        CHECK(lines[2].startsWith(QStringLiteral("inset.mp4 -- inset on “V1”")));
    }
    // Opening the project: every file the timeline uses, in the same order.
    paths.clear();
    for (const renderjobs::MissingSource &m :
         renderjobs::missingSources(t, renderjobs::MissingFor::Timeline))
        paths << m.path;
    CHECK((paths == QStringList{"/c/gone.insv", "/m/song.wav", "/m/quiet.wav", "/m/off.wav",
                                "/v/inset.mp4", "/v/hidden.mp4"}));

    // A take whose clip file is there but whose lens-1 file isn't (a ONE X2
    // clip): with the pool, the line names the missing file.
    QTemporaryDir tmp;
    const QString lead = tmp.filePath(QStringLiteral("VID_20250816_103807_00_001.insv"));
    const QString second = tmp.filePath(QStringLiteral("VID_20250816_103807_10_001.insv"));
    {
        QFile f(lead);
        CHECK(f.open(QIODevice::WriteOnly));
    }
    Timeline take;
    take.appendClip(lead, 5000, 25.0, 2880, 2880);
    take.setSourcesMissing({{lead, true}});
    MediaPool pool;
    MediaItem item{lead, MediaItem::Kind::Clip360, 5000};
    item.companions = {second};
    pool.add(item);
    const QStringList takeLines =
        renderjobs::missingSourceLines(take, renderjobs::MissingFor::Timeline, &pool);
    CHECK(takeLines.size() == 1);
    if (takeLines.size() == 1) {
        CHECK(takeLines[0].startsWith(QLatin1String("VID_20250816_103807_10_001.insv -- clip 1")));
        CHECK(takeLines[0].contains(second));
    }
}

void testClipJob(const QString &dir)
{
    Timeline t;
    const int id = t.appendClip("/c/ride.insv", 20000, 30.0);
    t.setClipTrimIn(id, 2000);
    t.setClipTrimOut(id, 9000);
    t.setClipSpeed(id, 2.0);
    ColorGrade g;
    g.exposureEv = 1.0f;
    t.setClipColor(id, g);
    t.setSourceSeamCorrection("/c/ride.insv", {{-0.5, 0.25, 0.1}}, 2);
    PrivacyBlur privacy;
    privacy.scans().setDirectory(dir);

    const ClipItem &c = t.clipAt(0);
    ReframeExportController::ClipJob job = renderjobs::clipJob(c, t, privacy);
    CHECK(job.clipPath == "/c/ride.insv" && job.trimInMs == 2000 && job.trimOutMs == 9000);
    CHECK(job.speed == 2.0 && job.colorGrade.adjust[0] == 1.0f);
    CHECK(job.seamCorrection.lens1RotDeg[1] == 0.25); // measured under Premium (2), the clip's
    CHECK(job.lensGuard == stitch::PreviewStitchConfig::LensGuard::Premium && job.depthSeam);
    CHECK(!job.stab);    // stabilization off
    CHECK(!job.poseAt);  // no keys: looking straight ahead
    CHECK(!job.blur);    // no blur
    t.setSourceStitch("/c/ride.insv", stitch::PreviewStitchConfig::LensGuard::Standard, false);
    job = renderjobs::clipJob(t.clipAt(0), t, privacy);
    CHECK(job.lensGuard == stitch::PreviewStitchConfig::LensGuard::Standard && !job.depthSeam);
    CHECK(job.seamCorrection.lens1RotDeg[1] == 0.0); // measured under another guard
    t.setSourceStitch("/c/ride.insv", stitch::PreviewStitchConfig::LensGuard::Premium, true);

    t.upsertClipReframeKey(id, 3000, {0.5f, 0.0f, 0.0f, 1.2f});
    job = renderjobs::clipJob(t.clipAt(0), t, privacy);
    CHECK(job.poseAt);
    if (job.poseAt) {
        view::Pose pose;
        job.poseAt(5000, pose);
        CHECK(near(pose.yaw, 0.5f) && near(float(pose.fov), 1.2f) && pose.distortionIsAuto());
    }
}

void testSuggestedPaths(const QString &dir)
{
    CHECK(renderjobs::suggestedExportPath(QString()) == QDir::homePath() + "/timeline.mp4");
    const QString file = projectfolder::create(dir, "Ride");
    CHECK(!file.isEmpty());
    const QString first = renderjobs::suggestedExportPath(file);
    CHECK(first == QDir(projectfolder::exportsDir(file)).filePath("Ride.mp4"));
    QFile f(first);
    CHECK(f.open(QIODevice::WriteOnly));
    f.close();
    CHECK(renderjobs::suggestedExportPath(file) ==
          QDir(projectfolder::exportsDir(file)).filePath("Ride-2.mp4"));

    const QString shot = renderjobs::suggestedScreenshotPath(file, "/c/ride.insv", 62003);
    CHECK(shot == QDir(projectfolder::screenshotsDir(file)).filePath("Ride_01m02s003.png"));
    CHECK(renderjobs::suggestedScreenshotPath(QString(), "/c/ride.insv", -5)
              .endsWith("/ride_00m00s000.png")); // unsaved: named after the clip
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv); // fonts, for the titles
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    testMix();
    testMissingSources();
    testOverlays();
    testTitles();
    testTitleMotion();
    testTypeOn();
    testTypeOnFeedVsExport();
    testClockFields();
    testSourceTimeMap();
    testClock();
    testClockFeedVsExport();
    testGpsFields();
    testGpsClock();
    testGpsFeedVsExport();
    testMotionClock();
    testTelemetryCache();
    testGaugeReadings();
    testGaugeSource();
    testGaugeFeedVsExport();
    testTransitions();
    testLayerOverMainClip();
    testClipJob(tmp.path());
    testSuggestedPaths(tmp.path());
    if (g_failures == 0)
        std::printf("render_jobs_test: all checks passed\n");
    else
        std::printf("render_jobs_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
