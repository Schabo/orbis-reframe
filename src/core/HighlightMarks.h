#pragma once

#include <QList>
#include <QtGlobal>

// Highlight marks, as a clip (ClipItem) and a media pool file (MediaItem)
// keep them: the user's own, in source ms, sorted, and the camera's the user
// hid, by their source ms, sorted. The camera's marks themselves are the
// file's (read from it, not kept here): each call is handed them as
// `camera`, empty where there are none (a flat video). Every edit returns
// true iff it changed something.
namespace highlight {

// How near a mark the playhead must be for Shift+M to delete or hide it:
// as near as for a reframe key (ClipItem::kReframeDeleteSnapMs).
constexpr qint64 kRemoveSnapMs = 150;

struct Mark {
    qint64 sourceMs = 0;
    bool own = false; // the user's, else the camera's
    bool operator==(const Mark &o) const { return sourceMs == o.sourceMs && own == o.own; }
};

// The marks shown, sorted: the user's and those of `camera` not hidden.
QList<Mark> shown(const QList<qint64> &own, const QList<qint64> &hidden,
                  const QList<qint64> &camera);

// One of the user's at `ms`, clamped to [0, durationMs]. Refused within
// `frameMs` of a shown mark; a hidden camera mark there is shown again
// instead.
bool add(QList<qint64> &own, QList<qint64> &hidden, qint64 ms, qint64 durationMs,
         qint64 frameMs, const QList<qint64> &camera);
// The shown mark nearest `ms`, within kRemoveSnapMs and [fromMs, toMs]: the
// user's is deleted, a camera one hidden.
bool removeNear(QList<qint64> &own, QList<qint64> &hidden, qint64 ms, qint64 fromMs,
                qint64 toMs, const QList<qint64> &camera);
// Hide or show the camera's mark at `ms`.
bool setHidden(QList<qint64> &hidden, qint64 ms, bool hide);
// `a` with `b`'s marks added, sorted, each once.
QList<qint64> merged(const QList<qint64> &a, const QList<qint64> &b);

} // namespace highlight
