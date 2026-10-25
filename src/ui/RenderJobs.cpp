#include "RenderJobs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>


#include "ClipViewSource.h"
#include "PrivacyBlur.h"
#include "TitleRender.h"
#include "TitleSource.h"
#include "../core/ClipItem.h"
#include "../core/MediaPool.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"

namespace renderjobs {

namespace {

// A title's largest text size, over its keys too.
float maxTitleSize(const TrackClip &c)
{
    float size = c.look.width;
    for (const InsetKey &k : c.insetKeys.keys())
        size = std::max(size, k.look.width);
    return size;
}

// Playback renders a title's text no bigger than this (the view is mostly
// smaller than the output); the export at the size it shows at, up to the
// second.
constexpr int kViewFontCapPx = 360;
constexpr int kExportFontCapPx = 2048;

// A title clip's pictures in an output of outW x outH, its text rendered at
// its largest size, at most `capPx`; `clock` fills in its fields.
std::shared_ptr<TitleSource> titleSource(const TrackClip &c, int outW, int outH, int capPx,
                                         const title::Clock &clock)
{
    return std::make_shared<TitleSource>(
        *c.title, c.motion, c.timelineDurationMs(),
        titlerender::fontPxFor(maxTitleSize(c), outH, capPx), outW, outH,
        title::hasFields(c.title->text) ? clock : title::Clock(), c.startMs);
}

// Is a title worth a layer: has it text to show, now or as its clock runs?
// `pic`: its picture at its start.
bool showsTitle(const TrackClip &c, const exportpipe::Still &pic)
{
    return !pic.empty() || title::hasFields(c.title->text);
}

// A title's picture's shape at its start (exportpipe::Still: each picture
// is placed by its own), 1 if it shows nothing then.
double titleAspect(const exportpipe::Still &pic)
{
    return pic.empty() ? 1.0 : double(pic.width) / pic.height;
}

// A clip's cameras on a job: pane 0's reframe track as poseAt (none: a
// static forward view), a split's other panes as panePoseAt.
void setViews(ReframeExportController::ClipJob &job, const ClipItem &c)
{
    if (!c.reframe.isEmpty())
        job.poseAt = [track = c.reframe](qint64 sourceMs, view::Pose &pose) {
            pose = track.sample(sourceMs);
        };
    if (c.paneCount() < 2)
        return;
    job.split = c.split;
    job.panePoseAt = [panes = c.panes](qint64 sourceMs, int pane, view::Pose &pose) {
        if (pane >= 1 && pane < pane::kMaxPanes && !panes[pane - 1].isEmpty())
            pose = panes[pane - 1].sample(sourceMs);
    };
}

// A 360 clip's view as a layer's pictures (ClipViewSource): what the export
// renders it with, its stabilizer built when the source opens.
ClipViewSource::Spec viewSpec(const ClipItem &c, const Timeline &timeline,
                              const PrivacyBlur &privacy)
{
    ClipViewSource::Spec spec;
    ReframeExportController::ClipJob &job = spec.job;
    job.clipPath = c.sourcePath;
    job.colorGrade = c.color.render();
    job.blur = privacy.blurFor(c);
    job.lensGuard = c.lensGuard;
    job.depthSeam = c.depthSeam;
    timeline.sourceSeamCorrection(c.sourcePath, job.seamCorrection);
    setViews(job, c);
    spec.buildStab = c.stabilization;
    spec.fps = c.fps;
    spec.syncMs = std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs;
    return spec;
}

// Everything a 360 clip's view depends on (viewSpec), as a string: the
// layer's InsetFeed::Clip::pictureKey.
QString viewKey(const ClipItem &c, const Timeline &timeline)
{
    insv::SeamCorrection seam;
    timeline.sourceSeamCorrection(c.sourcePath, seam);
    const insv::StabConfig &s = c.stabilization;
    QString key = QStringLiteral("%1|%2|%3,%4,%5,%6|%7,%8|%9,%10,%11")
                      .arg(c.id)
                      .arg(c.sourcePath)
                      .arg(c.color.exposureEv)
                      .arg(c.color.contrast)
                      .arg(c.color.saturation)
                      .arg(c.color.temperature)
                      .arg(int(c.lensGuard))
                      .arg(c.depthSeam)
                      .arg(seam.lens1RotDeg[0])
                      .arg(seam.lens1RotDeg[1])
                      .arg(seam.lens1RotDeg[2]);
    key += QStringLiteral("|%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11")
               .arg(int(s.mode))
               .arg(s.windowYawSec)
               .arg(s.windowPitchSec)
               .arg(s.windowRollSec)
               .arg(s.horizonLockAmount)
               .arg(s.horizonGain)
               .arg(s.accelLowPassHz)
               .arg(s.autoLevel)
               .arg(s.pitchOffsetRad)
               .arg(s.rollOffsetRad)
               .arg(std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs);
    if (s.lockDirection)
        key += QStringLiteral(",dir%1").arg(s.directionSec);
    key += QStringLiteral("|%1,%2,%3,%4").arg(c.blurPlates).arg(c.plateScan)
               .arg(c.blurRemovals.size()).arg(c.blurAdditions.size());
    for (int pane = 0; pane < c.paneCount(); ++pane) {
        if (pane > 0)
            key += QStringLiteral("|pane%1").arg(pane);
        for (const ReframeKey &k : c.paneTrack(pane).keys())
            key += QStringLiteral("|%1:%2,%3,%4,%5,%6,%7")
                       .arg(k.timeMs)
                       .arg(k.pose.yaw)
                       .arg(k.pose.pitch)
                       .arg(k.pose.roll)
                       .arg(k.pose.fov, 0, 'g', 17)
                       .arg(k.pose.distortion)
                       .arg(int(k.ease));
    }
    if (c.paneCount() > 1)
        key += QStringLiteral("|split%1,%2,%3")
                   .arg(int(c.split.layout))
                   .arg(c.split.gap)
                   .arg(c.split.gapRgb);
    return key;
}

// A dip to black's picture: a few opaque black pixels (premultiplied RGBA),
// stretched over the frame. One, shared: playback uploads it once.
constexpr int kBlackPx = 4;
std::shared_ptr<exportpipe::StillSource> blackStill()
{
    static const auto black = [] {
        auto px = std::make_shared<std::vector<uint8_t>>(kBlackPx * kBlackPx * 4, 0);
        for (size_t i = 3; i < px->size(); i += 4)
            (*px)[i] = 255;
        exportpipe::Still s;
        s.rgba = std::move(px);
        s.width = s.height = kBlackPx;
        return std::make_shared<exportpipe::FixedStill>(std::move(s));
    }();
    return black;
}

} // namespace

std::vector<TransitionLayer> transitionLayers(const Timeline &timeline)
{
    std::vector<TransitionLayer> layers;
    for (int i = 0; i + 1 < timeline.count(); ++i) {
        const ClipItem &a = timeline.clipAt(i);
        const ClipItem &b = timeline.clipAt(i + 1);
        const Transition *t = timeline.transitionAfter(a.id);
        if (!t)
            continue;
        const Timeline::TransitionSpan span = timeline.transitionSpan(*t);
        if (!span.plays())
            continue;
        const qint64 half = span.durationMs / 2;
        if (!t->usesHandles()) { // black, in and out again
            TransitionLayer in;
            in.id = -2 * a.id;
            in.overClipId = a.id;
            in.startMs = span.cutMs - half;
            in.trimOutMs = half;
            in.rampFrom = 0.0f;
            in.rampTo = 1.0f;
            TransitionLayer out;
            out.id = -2 * a.id - 1;
            out.overClipId = b.id;
            out.startMs = span.cutMs;
            out.trimOutMs = half;
            out.rampFrom = 1.0f;
            out.rampTo = 0.0f;
            layers.push_back(in);
            layers.push_back(out);
            continue;
        }
        TransitionLayer in; // b, blending in up to the cut
        in.id = -2 * a.id;
        in.clipIndex = i + 1;
        in.overClipId = a.id;
        in.startMs = span.cutMs - half;
        in.trimInMs = b.trimInMs - std::llround(half * b.speed);
        in.trimOutMs = b.trimInMs;
        in.rampFrom = 0.0f;
        in.rampTo = 0.5f;
        TransitionLayer out; // a, blending out after it
        out.id = -2 * a.id - 1;
        out.clipIndex = i;
        out.overClipId = b.id;
        out.startMs = span.cutMs;
        out.trimInMs = a.trimOutMs;
        out.trimOutMs = a.trimOutMs + std::llround(half * a.speed);
        out.rampFrom = 0.5f;
        out.rampTo = 0.0f;
        layers.push_back(in);
        layers.push_back(out);
    }
    return layers;
}

std::vector<insv::MixClip> mixClips(const Timeline &timeline)
{
    constexpr int kFramesPerMs = 48; // playback and export both mix at 48 kHz
    // Each transition's half span, by the clip it runs on past (its end at
    // the cut) and by the one it starts early (its start at the cut).
    struct Cross {
        qint64 cutMs = 0;
        qint64 half = 0;
        bool handles = true; // a crossfade's: past the cut; a dip's: up to it
    };
    std::map<int, Cross> pastEnd, beforeStart;
    for (const Transition &t : timeline.transitions()) {
        const Timeline::TransitionSpan span = timeline.transitionSpan(t);
        if (span.plays()) {
            pastEnd[t.leftClipId] = {span.cutMs, span.durationMs / 2, t.usesHandles()};
            beforeStart[t.rightClipId] = {span.cutMs, span.durationMs / 2, t.usesHandles()};
        }
    }
    std::vector<insv::MixClip> mix;
    for (int i = 0; i < timeline.trackCount(); ++i) {
        const Track &t = timeline.trackAt(i);
        if (t.muted || t.kind != Track::Kind::Audio)
            continue;
        for (const TrackClip &c : t.clips) {
            if (c.muted || c.mediaMissing)
                continue;
            insv::MixClip m;
            m.path = c.sourcePath.toStdString();
            m.stream = c.stream;
            m.startFrame = c.startMs * kFramesPerMs;
            m.lengthFrames = c.timelineDurationMs() * kFramesPerMs;
            m.inFrame = c.trimInMs * kFramesPerMs;
            m.speed = c.speed;
            m.gain = static_cast<float>(std::pow(10.0, (c.gainDb + t.gainDb) / 20.0));
            m.fadeInFrames = c.fadeInMs * kFramesPerMs;
            m.fadeOutFrames = c.fadeOutMs * kFramesPerMs;
            // A clip's own sound at a transition, its edge on the cut (a
            // couple of ms of rounding either way). A crossfade's: on past it
            // / from before it, fading over the whole transition. A dip's:
            // fading out up to it / in from it, over half (or the clip's own
            // fade, if longer).
            constexpr qint64 kOnCutMs = 2;
            if (const auto x = pastEnd.find(c.linkedClipId);
                x != pastEnd.end() && std::abs(c.endMs() - x->second.cutMs) <= kOnCutMs) {
                const qint64 half = x->second.half;
                if (x->second.handles) {
                    m.lengthFrames += half * kFramesPerMs;
                    m.fadeOutFrames = 2 * half * kFramesPerMs;
                } else {
                    m.fadeOutFrames = std::max<int64_t>(m.fadeOutFrames, half * kFramesPerMs);
                }
            }
            if (const auto x = beforeStart.find(c.linkedClipId);
                x != beforeStart.end() && std::abs(c.startMs - x->second.cutMs) <= kOnCutMs) {
                const qint64 half = x->second.half;
                if (x->second.handles) {
                    m.startFrame -= half * kFramesPerMs;
                    m.inFrame -= std::llround(half * c.speed) * kFramesPerMs;
                    m.lengthFrames += half * kFramesPerMs;
                    m.fadeInFrames = 2 * half * kFramesPerMs;
                } else {
                    m.fadeInFrames = std::max<int64_t>(m.fadeInFrames, half * kFramesPerMs);
                }
            }
            m.noise = c.noise;
            mix.push_back(std::move(m));
        }
    }
    return mix;
}

title::Clock titleClock(const Timeline &timeline, const RecordedTimes &recorded)
{
    std::vector<title::Clock::Span> spans;
    qint64 at = 0;
    for (int i = 0; i < timeline.count(); ++i) {
        const ClipItem &c = timeline.clipAt(i);
        title::Clock::Span s;
        s.startMs = at;
        s.endMs = at + c.timelineDurationMs();
        s.recorded = recorded.value(c.sourcePath);
        s.trimInMs = c.trimInMs;
        s.speed = c.speed;
        spans.push_back(s);
        at = s.endMs;
    }
    return title::Clock(std::move(spans));
}

bool anyTitleClock(const Timeline &timeline)
{
    for (int i = 0; i < timeline.trackCount(); ++i)
        for (const TrackClip &c : timeline.trackAt(i).clips)
            if (c.isTitle() && title::hasFields(c.title->text))
                return true;
    return false;
}

std::vector<InsetFeed::Clip> insetFeed(const Timeline &timeline, int outW, int outH,
                                       const PrivacyBlur &privacy, const RecordedTimes &recorded)
{
    const title::Clock clock = titleClock(timeline, recorded);
    std::vector<InsetFeed::Clip> clips;
    for (int i = 0; i < timeline.count(); ++i) {
        const ClipItem &c = timeline.clipAt(i);
        if (!c.flat || c.mediaMissing)
            continue;
        InsetFeed::Clip f;
        f.id = c.id;
        f.path = c.sourcePath;
        f.startMs = timeline.clipToTimeline(i, 0);
        f.trimInMs = c.trimInMs;
        f.trimOutMs = c.trimOutMs;
        f.speed = c.speed;
        f.aspect = c.sourceHeight > 0 ? double(c.sourceWidth) / c.sourceHeight : 16.0 / 9.0;
        f.look = inset::fitted(f.aspect, outW, outH, c.fit == ClipItem::Fit::Fill);
        f.fullFrame = true;
        f.colorGrade = c.color.render();
        clips.push_back(std::move(f));
    }
    // The transitions' other clips and black, over the main track, under the
    // overlays.
    for (const TransitionLayer &l : transitionLayers(timeline)) {
        if (l.clipIndex < 0) {
            if (outW <= 0 || outH <= 0)
                continue;
            InsetFeed::Clip f;
            f.id = l.id;
            f.startMs = l.startMs;
            f.trimInMs = l.trimInMs;
            f.trimOutMs = l.trimOutMs;
            f.rampFrom = l.rampFrom;
            f.rampTo = l.rampTo;
            f.overMainClipId = l.overClipId;
            f.fullFrame = true;
            f.aspect = double(outW) / outH;
            f.look = inset::fitted(f.aspect, outW, outH, false);
            f.still = blackStill();
            clips.push_back(std::move(f));
            continue;
        }
        const ClipItem &c = timeline.clipAt(l.clipIndex);
        if (c.mediaMissing)
            continue;
        InsetFeed::Clip f;
        f.id = l.id;
        f.path = c.sourcePath;
        f.startMs = l.startMs;
        f.trimInMs = l.trimInMs;
        f.trimOutMs = l.trimOutMs;
        f.speed = c.speed;
        f.rampFrom = l.rampFrom;
        f.rampTo = l.rampTo;
        f.overMainClipId = l.overClipId;
        f.fullFrame = true;
        if (c.flat) {
            f.aspect = c.sourceHeight > 0 ? double(c.sourceWidth) / c.sourceHeight : 16.0 / 9.0;
            f.look = inset::fitted(f.aspect, outW, outH, c.fit == ClipItem::Fit::Fill);
            f.colorGrade = c.color.render();
        } else if (outW > 0 && outH > 0) {
            f.aspect = double(outW) / outH;
            f.look = inset::fitted(f.aspect, outW, outH, false);
            f.picture = [spec = viewSpec(c, timeline, privacy)] {
                return std::make_unique<ClipViewSource>(spec);
            };
            f.pictureKey = viewKey(c, timeline);
        } else {
            continue;
        }
        clips.push_back(std::move(f));
    }
    // Bottom-most first: the lowest overlay row draws first, the top row last.
    for (int i = timeline.trackCount() - 1; i >= 0; --i) {
        const Track &t = timeline.trackAt(i);
        if (t.kind != Track::Kind::Video || t.muted)
            continue; // (hidden)
        for (const TrackClip &c : t.clips) {
            if (c.mediaMissing)
                continue;
            std::shared_ptr<TitleSource> title;
            exportpipe::Still pic;
            if (c.isTitle()) {
                title = titleSource(c, outW, outH, kViewFontCapPx, clock);
                pic = title->full();
                if (!showsTitle(c, pic) || outW <= 0)
                    continue; // no text
            }
            InsetFeed::Clip f;
            f.id = c.id;
            f.path = c.sourcePath;
            f.startMs = c.startMs;
            f.trimInMs = c.trimInMs;
            f.trimOutMs = c.trimOutMs;
            f.speed = c.speed;
            f.aspect = c.aspect();
            f.fadeInMs = c.fadeInMs;
            f.fadeOutMs = c.fadeOutMs;
            f.look = c.look;
            f.frame = c.frame;
            f.keys = c.insetKeys;
            f.motion = c.motion;
            f.outAspect = outH > 0 ? double(outW) / outH : 16.0 / 9.0;
            if (title) {
                f.still = title;
                f.aspect = titleAspect(pic);
                f.frame = {};
            }
            clips.push_back(std::move(f));
        }
    }
    return clips;
}

double titleAnchorShift(const title::Style &from, const title::Style &to, int outW, int outH,
                        const QDateTime &at)
{
    if (from.align == to.align || outW <= 0 || outH <= 0)
        return 0.0;
    // The picture's shape doesn't depend on its size: any size will do.
    constexpr int kFontPx = 200;
    const auto shown = [&at](title::Style s) {
        s.text = title::withFields(s.text, at, QLocale(), s.hours);
        return s;
    };
    const titlerender::Picture a =
        titlerender::render(shown(from), kFontPx, -1, title::hasFields(from.text));
    const titlerender::Picture b =
        titlerender::render(shown(to), kFontPx, -1, title::hasFields(to.text));
    if (a.empty() || b.empty())
        return 0.0;
    const double aspect = double(outH) / outW;
    // Keep the drawn centre: x + (0.5 - anchor) * widthPerSize the same.
    return (0.5 - a.anchorX) * a.widthPerFontPx * aspect -
           (0.5 - b.anchorX) * b.widthPerFontPx * aspect;
}

std::vector<exportpipe::InsetClipJob> insetJobs(const Timeline &timeline, int outW, int outH,
                                                const PrivacyBlur &privacy,
                                                const RecordedTimes &recorded)
{
    const title::Clock clock = titleClock(timeline, recorded);
    std::vector<exportpipe::InsetClipJob> jobs;
    // The flat main-track clips first, then the transitions' other clips,
    // under the overlays -- as insetFeed.
    for (const InsetFeed::Clip &f : insetFeed(timeline, outW, outH, privacy, recorded)) {
        if (!f.fullFrame)
            break; // the overlays: below
        exportpipe::InsetClipJob j;
        j.path = f.path.toStdString();
        j.startMs = f.startMs;
        j.trimInMs = f.trimInMs;
        j.trimOutMs = f.trimOutMs;
        j.speed = f.speed;
        j.aspect = f.aspect;
        j.colorGrade = f.colorGrade;
        j.rampFrom = f.rampFrom;
        j.rampTo = f.rampTo;
        j.picture = f.picture;
        j.still = f.still; // a dip's black
        j.lookAt = [look = f.look](int64_t) { return look; };
        jobs.push_back(std::move(j));
    }
    for (int i = timeline.trackCount() - 1; i >= 0; --i) {
        const Track &t = timeline.trackAt(i);
        if (t.kind != Track::Kind::Video || t.muted)
            continue;
        for (const TrackClip &c : t.clips) {
            if (c.mediaMissing)
                continue;
            std::shared_ptr<TitleSource> title;
            exportpipe::Still pic;
            if (c.isTitle()) {
                title = titleSource(c, outW, outH, kExportFontCapPx, clock);
                pic = title->full();
                if (!showsTitle(c, pic) || outW <= 0)
                    continue; // no text
            }
            exportpipe::InsetClipJob j;
            j.path = c.sourcePath.toStdString();
            j.startMs = c.startMs;
            j.trimInMs = c.trimInMs;
            j.trimOutMs = c.trimOutMs;
            j.speed = c.speed;
            j.aspect = c.aspect();
            j.fadeInMs = c.fadeInMs;
            j.fadeOutMs = c.fadeOutMs;
            j.frame = c.frame;
            j.motion = c.motion;
            // A copy: the export thread samples it.
            j.lookAt = [look = c.look, keys = c.insetKeys](int64_t sourceMs) {
                return keys.isEmpty() ? look : keys.sample(sourceMs);
            };
            if (title) {
                // The look's width is the text size, its centreX the text's
                // anchor: each picture is placed by its own numbers, as
                // playback places it.
                j.still = title;
                j.aspect = titleAspect(pic);
                j.frame = {};
            }
            jobs.push_back(std::move(j));
        }
    }
    return jobs;
}

ReframeExportController::ClipJob clipJob(const ClipItem &c, const Timeline &timeline,
                                         const PrivacyBlur &privacy)
{
    // Stabilization (Lean follow / Horizon Lock / Custom) is a per-frame
    // sphere-frame matrix next to the reframe pose, applied after it like the
    // viewport's pc.stab -- the native stitch itself is never stabilized.
    const bool nativeStab = c.stabilization.mode != insv::StabMode::Off;
    ReframeExportController::ClipJob job;
    job.flat = c.flat;
    job.clipPath = c.sourcePath;
    job.trimInMs = c.trimInMs;
    job.trimOutMs = c.trimOutMs;
    job.speed = c.speed;
    job.colorGrade = c.color.render();
    job.blur = privacy.blurFor(c);
    job.lensGuard = c.lensGuard;
    job.depthSeam = c.depthSeam;
    timeline.sourceSeamCorrection(c.sourcePath, job.seamCorrection);
    if (nativeStab)
        job.stab = ReframeExportController::buildStabilizer(
            c.sourcePath, c.stabilization, std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs);
    setViews(job, c); // no keyframes -> a static forward view
    return job;
}

QList<MissingSource> missingSources(const Timeline &timeline, MissingFor which)
{
    const auto tr = [](const char *text) { return QCoreApplication::translate("RenderJobs", text); };
    QList<MissingSource> out;
    const auto add = [&out](const QString &path, const QString &place) {
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const MissingSource &m) { return m.path == path; });
        if (it == out.end())
            it = out.insert(out.end(), MissingSource{path, {}});
        it->places << place;
    };
    for (int i = 0; i < timeline.count(); ++i)
        if (const ClipItem &c = timeline.clipAt(i); c.mediaMissing)
            add(c.sourcePath, tr("clip %1").arg(i + 1));
    for (int i = 0; i < timeline.trackCount(); ++i) {
        const Track &t = timeline.trackAt(i);
        // A hidden video track, a muted audio track or clip: not exported.
        const bool all = which == MissingFor::Timeline;
        if (t.muted && !all)
            continue;
        for (const TrackClip &c : t.clips)
            if (c.mediaMissing && !c.isTitle() &&
                (all || !(t.kind == Track::Kind::Audio && c.muted)))
                add(c.sourcePath, t.kind == Track::Kind::Video
                                      ? tr("inset on “%1”").arg(t.name)
                                      : tr("sound on “%1”").arg(t.name));
    }
    return out;
}

QStringList missingSourceLines(const Timeline &timeline, MissingFor which,
                               const MediaPool *pool)
{
    QStringList out;
    for (const MissingSource &m : missingSources(timeline, which)) {
        QString file = m.path;
        if (const MediaItem *item = pool ? pool->item(m.path) : nullptr)
            if (const QString f = media::firstMissingFile(*item); !f.isEmpty())
                file = f;
        out << QStringLiteral("%1 -- %2\n    %3")
                   .arg(QFileInfo(file).fileName(), m.places.join(QLatin1String(", ")), file);
    }
    return out;
}

QString suggestedExportPath(const QString &projectFile)
{
    if (projectFile.isEmpty())
        return QDir::homePath() + QStringLiteral("/timeline.mp4");
    QString dir = projectfolder::ensure(projectfolder::exportsDir(projectFile));
    if (dir.isEmpty())
        dir = QFileInfo(projectFile).absolutePath();
    const QString base = QFileInfo(projectFile).completeBaseName();
    QString suggested = QDir(dir).filePath(base + QStringLiteral(".mp4"));
    for (int n = 2; QFileInfo::exists(suggested); ++n)
        suggested = QDir(dir).filePath(QStringLiteral("%1-%2.mp4").arg(base).arg(n));
    return suggested;
}

QString suggestedScreenshotPath(const QString &projectFile, const QString &clipSource,
                                qint64 timelineMs)
{
    QString dir;
    QString base;
    if (!projectFile.isEmpty()) {
        const QFileInfo fi(projectFile);
        dir = projectfolder::ensure(projectfolder::screenshotsDir(projectFile));
        if (dir.isEmpty())
            dir = fi.absolutePath();
        base = fi.completeBaseName();
    } else {
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (dir.isEmpty() || !QDir(dir).exists())
            dir = QDir::homePath();
        base = QFileInfo(clipSource).completeBaseName();
    }
    const qint64 t = std::max<qint64>(0, timelineMs);
    const QString stamp = QStringLiteral("%1m%2s%3")
                              .arg(t / 60000, 2, 10, QLatin1Char('0'))
                              .arg(t / 1000 % 60, 2, 10, QLatin1Char('0'))
                              .arg(t % 1000, 3, 10, QLatin1Char('0'));
    QString suggested = QDir(dir).filePath(QStringLiteral("%1_%2.png").arg(base, stamp));
    for (int n = 2; QFileInfo::exists(suggested); ++n)
        suggested = QDir(dir).filePath(QStringLiteral("%1_%2-%3.png").arg(base, stamp).arg(n));
    return suggested;
}

} // namespace renderjobs
