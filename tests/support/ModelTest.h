#pragma once

// What the edit-model tests (clip_item_test, timeline_test, ...) share: the
// src/core headers, CHECK, which counts failures, and a few helpers that
// build clips. Each test's main() ends with `return modeltest::finish(name)`:
// the exit code is the failure count.

#include "Autosave.h"
#include "ClipItem.h"
#include "MediaPool.h"
#include "Project.h"
#include "ProjectFolder.h"
#include "ProjectLibrary.h"
#include "Timeline.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace modeltest {

inline int g_failures = 0;

inline int finish(const char *name)
{
    if (g_failures == 0)
        std::printf("%s: all checks passed\n", name);
    else
        std::printf("%s: %d check(s) failed\n", name, g_failures);
    return g_failures;
}

} // namespace modeltest

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++modeltest::g_failures;                                                               \
        }                                                                                          \
    } while (0)

inline void writeFile(const QString &path, const QByteArray &data = QByteArrayLiteral("x"))
{
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(data);
    f.close();
}

// A second mic stream linked to its video clip, as the clip's own sound is.
inline int addLinkedMic(Timeline &tl, int trackId, int videoId)
{
    const ClipItem *v = tl.clipById(videoId);
    TrackClip a;
    a.sourcePath = v->sourcePath;
    a.sourceDurationMs = v->sourceDurationMs;
    a.stream = 1;
    a.trimInMs = v->trimInMs;
    a.trimOutMs = v->trimOutMs;
    a.speed = v->speed;
    a.linkedClipId = videoId;
    return tl.insertTrackClip(trackId, a);
}

// v8: video tracks round-trip through the .orp.
// An overlay's sound on audio track `trackId`, linked to it (as
// Timeline::addOverlaySound does on the overlay's own audio track).
inline int addOverlayAudio(Timeline &tl, int trackId, int overlayId)
{
    const TrackClip *o = tl.trackClipById(overlayId);
    TrackClip a;
    a.sourcePath = o->sourcePath;
    a.sourceDurationMs = o->sourceDurationMs;
    a.startMs = o->startMs;
    a.trimInMs = o->trimInMs;
    a.trimOutMs = o->trimOutMs;
    a.linkedClipId = overlayId;
    return tl.insertTrackClip(trackId, a);
}
