#pragma once

#include <QList>

#include <cstdint>

// The authored pan/tilt/zoom path for a clip: a time-ordered set of camera
// pose keyframes that the viewport follows during playback and scrubbing.
// The key search / insert / remove is keytrack:: (KeyTrack.h), shared with
// InsetKeyTrack; the interpolation is this class's own.
//
// Pure value type (no QObject) so it's trivially testable and
// serialisable. Each ClipItem holds one; Timeline wraps every mutation in a
// signal.
//
// yaw and roll are unbounded radians (the reprojection wraps them via cos/sin),
// so interpolation takes the shortest angular path across the +/-pi seam. pitch
// and fovY are plain lerp -- both are clamped ranges with no wraparound.

struct ReframePose {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;   // rotation about the view axis; horizon tilt
    float fovY = 1.309f; // ~75 deg, matches EquirectViewportWindow's default
};

struct ReframeKey {
    qint64 timeMs = 0;
    ReframePose pose;
};

class ReframeTrack {
public:
    bool isEmpty() const { return m_keys.isEmpty(); }
    int count() const { return static_cast<int>(m_keys.size()); }
    const QList<ReframeKey> &keys() const { return m_keys; }
    QList<qint64> keyTimes() const;

    // Insert a key at timeMs, or overwrite the pose of an existing key whose
    // time is within snapMs of timeMs (so re-authoring at "the same" playhead
    // frame updates rather than piles up near-duplicates).
    void upsert(qint64 timeMs, const ReframePose &pose, qint64 snapMs);

    // Remove the key nearest timeMs if one is within snapMs. Returns true if a
    // key was removed.
    bool removeNear(qint64 timeMs, qint64 snapMs);

    void clear() { m_keys.clear(); }

    // Drop every key in [fromMs, toMs] and put `keys` there instead (a tracked
    // path replacing that span). Keys outside the span are clamped into it;
    // keys sharing a time keep the last. Keys outside the span stay.
    void replaceSpan(qint64 fromMs, qint64 toMs, const QList<ReframeKey> &keys);

    // Pose at timeMs: the first key's pose at or before the first key, the last
    // key's pose at or after the last, otherwise interpolated between the two
    // bracketing keys. Returns a default pose if the track is empty (callers are
    // expected to check isEmpty() and free-look instead).
    ReframePose sample(qint64 timeMs) const;

private:
    QList<ReframeKey> m_keys; // sorted by timeMs, unique times
};
