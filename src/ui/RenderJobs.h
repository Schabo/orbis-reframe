#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>

#include <vector>

#include <AudioMixer.h>      // insv::MixClip
#include <InsetCompositor.h> // exportpipe::InsetClipJob

#include "InsetFeed.h"               // InsetFeed::Clip
#include "ReframeExportController.h" // ReframeExportController::ClipJob
#include "../core/TitleClock.h"

struct ClipItem;
class MediaPool;
class PrivacyBlur;
class Timeline;

// The edit model as the renderers want it: what playback, the export and a
// screenshot are handed. Pure functions of the Timeline (and the privacy
// blur), on the GUI thread; what they return is safe to use on another.
namespace renderjobs {

// The audio tracks as the mixer wants them (muted tracks / clips and missing
// files left out). At a transition (Transition.h) the sound linked to its
// two clips crossfades over it: the clip before's runs on past the cut and
// fades out, the clip after's starts early and fades in. Playback and export
// both use this.
std::vector<insv::MixClip> mixClips(const Timeline &timeline);

// When each source file was recorded (recordedTime), by its path: what a
// title's clock reads. Looked up on the GUI thread (the files' media
// information), only for the main track's clips.
using RecordedTimes = QHash<QString, QDateTime>;
// What titles' fields read from the main track's files, by path: when each
// was recorded, and its GPS telemetry (camera::ReadClipGps). Either is
// gathered only when a title has a field that reads it.
struct FieldSources {
    RecordedTimes recorded;
    title::Clock::Telemetries gps;
};

// The overlay clips (hidden tracks and missing files left out), bottom-most
// first: for playback's InsetFeed, and for the export / a screenshot. Under
// them, the flat main-track clips (ClipItem::flat) as full-frame layers,
// fitted into an output of outW x outH, and over those each transition's
// other clip (transitionLayers): a 360 clip's view through `privacy`'s
// blur, or a flat clip. A title's fields read `sources`.
std::vector<InsetFeed::Clip> insetFeed(const Timeline &timeline, int outW, int outH,
                                       const PrivacyBlur &privacy,
                                       const FieldSources &sources = {});
std::vector<exportpipe::InsetClipJob> insetJobs(const Timeline &timeline, int outW, int outH,
                                                const PrivacyBlur &privacy,
                                                const FieldSources &sources = {});

// Which file plays where on the timeline, and how far into it: each
// main-track clip's span with its trim and speed.
SourceTimeMap sourceTimeMap(const Timeline &timeline);
// What the titles' fields read along the main track: sourceTimeMap with
// each file's recording start and GPS (`sources`; a file not there has
// none).
title::Clock titleClock(const Timeline &timeline, const FieldSources &sources);
// Does any title on the timeline have time or date fields
// (title::hasClockFields)? Only then are the recording times worth looking
// up.
bool anyTitleClock(const Timeline &timeline);
// ... GPS fields (title::hasGpsFields)? Only then are the GPS tracks read.
bool anyTitleGps(const Timeline &timeline);

// How far a title's position has to move, in its text size times output
// widths (Timeline::setTrackClipTitle's shiftPerSize), for its text to stay
// where it is as its style changes from `from` to `to`: 0 unless the
// alignment changes, since the alignment says which point of the text the
// position pins (title::drawnLook). Their fields read `at` and `gps`.
double titleAnchorShift(const title::Style &from, const title::Style &to, int outW, int outH,
                        const QDateTime &at = QDateTime(),
                        const std::optional<Telemetry::Values> &gps = std::nullopt);

// What a transition shows besides the main track, in timeline order. A
// crossfade: over the first half of it the clip after the cut, from before
// its trim-in, blending in to half; over the second half the clip before
// the cut, past its trim-out, blending out from half. The main track plays
// on as it is, so with the layer over it the two clips blend through the
// cut. A dip to black: black, coming in up to the cut and going after it. A
// transition that doesn't play (Timeline::transitionSpan) has none.
struct TransitionLayer {
    int id = 0;          // unique among the layers, and not a clip's id (< 0)
    int clipIndex = -1;  // the main-track clip it shows; -1: black
    int overClipId = 0;  // the main-track clip it lies over (ClipItem::id)
    qint64 startMs = 0;  // timeline
    qint64 trimInMs = 0; // its source
    qint64 trimOutMs = 0;
    float rampFrom = 0.0f; // opacity at its start and end
    float rampTo = 0.0f;
};
std::vector<TransitionLayer> transitionLayers(const Timeline &timeline);

// One main-track clip as the export renders it: trim, speed, grade, the
// privacy blur, its stitch settings and the seam fine-tune measured under
// them, a stabilizer built from its IMU track (when its stabilization is on;
// reads the trailer), and its reframe path.
ReframeExportController::ClipJob clipJob(const ClipItem &c, const Timeline &timeline,
                                         const PrivacyBlur &privacy);

// A source file an export needs that is missing, and where the timeline
// uses it: "clip 2", "inset on “Overlay 1”", "sound on “Camera mic”".
struct MissingSource {
    QString path;
    QStringList places;
};
// Which missing files count: those an export needs (clips on hidden or muted
// tracks and muted audio clips aren't exported), or every one the timeline
// uses (opening a project: unhiding a track later doesn't warn again).
enum class MissingFor { Export, Timeline };
// The source files that are missing (ClipItem::mediaMissing,
// TrackClip::mediaMissing as last checked), each once, in timeline order:
// main-track clips first, then overlay and audio clips.
QList<MissingSource> missingSources(const Timeline &timeline, MissingFor which);
// The same as display lines, "name.ext -- clip 2, clip 3" with the full path
// indented under it: with several gone at once, bare paths don't say which
// clip is which.
// With `pool`, a take's line names the file of it that is missing (a ONE X2
// clip's lens-1 file), not the clip file.
QStringList missingSourceLines(const Timeline &timeline, MissingFor which,
                               const MediaPool *pool = nullptr);

// Where an export of the project in `projectFile` is suggested: its
// exports/ folder (made if need be; else the project file's own folder),
// named after the project, with -2, -3, ... if that's taken; ~/timeline.mp4
// for an unsaved project.
QString suggestedExportPath(const QString &projectFile);
// Where a screenshot at `timelineMs` is suggested: the project's
// screenshots/ folder (made if need be; else the project file's own
// folder), named after the project; Pictures (else home) while the project
// is unsaved, named after `clipSource`. With the time ("_01m02s003") and a
// -2, -3, ... if the name is taken.
QString suggestedScreenshotPath(const QString &projectFile, const QString &clipSource,
                                qint64 timelineMs);

} // namespace renderjobs
