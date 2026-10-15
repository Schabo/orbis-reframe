#pragma once

#include <QString>

#include <vector>

#include <AudioMixer.h>      // insv::MixClip
#include <InsetCompositor.h> // exportpipe::InsetClipJob

#include "InsetFeed.h"               // InsetFeed::Clip
#include "ReframeExportController.h" // ReframeExportController::ClipJob

struct ClipItem;
class PrivacyBlur;
class Timeline;

// The edit model as the renderers want it: what playback, the export and a
// screenshot are handed. Pure functions of the Timeline (and the privacy
// blur), on the GUI thread; what they return is safe to use on another.
namespace renderjobs {

// The audio tracks as the mixer wants them (muted tracks / clips and missing
// files left out). Playback and export both use this.
std::vector<insv::MixClip> mixClips(const Timeline &timeline);

// The overlay clips (hidden tracks and missing files left out), bottom-most
// first: for playback's InsetFeed, and for the export / a screenshot.
std::vector<InsetFeed::Clip> insetFeed(const Timeline &timeline);
std::vector<exportpipe::InsetClipJob> insetJobs(const Timeline &timeline);

// One main-track clip as the export renders it: trim, speed, grade, the
// privacy blur, the seam fine-tune measured under `lensGuard`, a stabilizer
// built from its IMU track (when its stabilization is on; reads the
// trailer), and its reframe path.
ReframeExportController::ClipJob clipJob(const ClipItem &c, const Timeline &timeline,
                                         int lensGuard, const PrivacyBlur &privacy);

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
