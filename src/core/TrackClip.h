#pragma once

#include "AudioLimits.h"
#include "InsetGeometry.h"
#include <NoiseReduction.h> // insv::NoiseReduction (header-only, no deps)

#include <QList>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Keyframes of a video-track clip's inset look: position, size and opacity over
// the clip's source time. Same shape as ReframeTrack; the key search / insert /
// remove is keytrack:: (KeyTrack.h).
struct InsetKey {
    qint64 timeMs = 0; // source time
    inset::Look look;
};

class InsetKeyTrack {
public:
    bool isEmpty() const { return m_keys.isEmpty(); }
    int count() const { return static_cast<int>(m_keys.size()); }
    const QList<InsetKey> &keys() const { return m_keys; }
    // Insert a key, or overwrite the one within snapMs of timeMs.
    void upsert(qint64 timeMs, const inset::Look &look, qint64 snapMs);
    // Remove the key nearest timeMs within snapMs. True if one was removed.
    bool removeNear(qint64 timeMs, qint64 snapMs);
    void clear() { m_keys.clear(); }
    // Held before the first key and after the last, linear between.
    // Default look if empty.
    inset::Look sample(qint64 timeMs) const;
    bool operator==(const InsetKeyTrack &o) const;

private:
    QList<InsetKey> m_keys; // sorted by timeMs, unique times
};

// One clip on an extra track, placed at a free position on the timeline. Unlike
// the main track's ClipItems, which sit back to back, a track clip carries its
// own timeline start. On an audio track it is a span of an audio file -- a
// music file (mp3 / wav / aac / flac / ...), or one audio stream of an .insv
// clip (its camera mic or external mic, linked to the video clip). On a video
// track it is a span of a flat video file shown as an inset over the output;
// the audio fields (stream, level, link) don't apply, and the fades fade its
// opacity. Its sound, if wanted, is a linked clip of the same file on an audio
// track.
//
// Plain value type like ClipItem: Timeline owns it and turns every successful
// change into a signal; the mutators here clamp, apply, and report whether
// anything changed.
struct TrackClip {
    static constexpr qint64 kMinTrimMs = 100;

    int id = 0; // unique across the whole Timeline (main clips included)
    QString sourcePath;
    qint64 sourceDurationMs = 0;
    bool mediaMissing = false;
    // Which audio stream of the source: ordinal among its audio streams, -1 =
    // the default one (insv::InsvAudioDemux::open's convention).
    int stream = -1;

    qint64 startMs = 0; // timeline position of the clip's first sample
    qint64 trimInMs = 0; // source time
    qint64 trimOutMs = 0;
    // Playback speed, pitch kept (as ClipItem::speed). Always 1 for a music
    // file; a clip's own stream keeps its video clip's speed so it stays in
    // step with the picture.
    double speed = 1.0;

    // Linked to a main-track video clip (each of its streams, put on the
    // stream tracks when the clip is added), or to an overlay clip (set when its sound is put on an audio track). Then
    // it follows that clip: `speed` is the clip's, and `startMs` is derived --
    // the clip's timeline start, plus where `trimInMs` falls in the clip's
    // source range, plus `syncOffsetMs` (set when linking, from where the clip
    // was: a linked clip can't be moved in time itself). Trimming the clip
    // moves the audio edges that were aligned with it; removing it removes the
    // audio. 0 = free (a music clip, or unlinked).
    int linkedClipId = 0;
    qint64 syncOffsetMs = 0;

    double gainDb = 0.0;
    bool muted = false;
    // Linear fades at the clip's timeline edges, ms. Clamped so the two
    // together never exceed the clip's span.
    qint64 fadeInMs = 0;
    qint64 fadeOutMs = 0;
    // Noise reduction on this clip's sound (audio tracks only), as
    // ClipItem::noise.
    insv::NoiseReduction noise;

    // --- video tracks only ---
    int sourceWidth = 0; // display size of the video (its aspect sizes the inset)
    int sourceHeight = 0;
    inset::Look look;        // where it sits, when it has no keyframes
    inset::Frame frame;      // corner radius / border
    InsetKeyTrack insetKeys; // keyframed look over source time

    double aspect() const
    {
        return sourceWidth > 0 && sourceHeight > 0 ? double(sourceWidth) / sourceHeight : 16.0 / 9.0;
    }
    // The look at source time `sourceMs`, before the fades.
    inset::Look lookAt(qint64 sourceMs) const
    {
        return insetKeys.isEmpty() ? look : insetKeys.sample(sourceMs);
    }

    qint64 timelineDurationMs() const
    {
        return std::llround((trimOutMs - trimInMs) / speed);
    }
    qint64 endMs() const { return startMs + timelineDurationMs(); }
    qint64 sourceAtLocalMs(qint64 localMs) const { return trimInMs + std::llround(localMs * speed); }

    // The whole source, trims reset, fades cleared.
    void resetToFullSource();

    // --- mutations: clamp, apply, return true iff something changed ---------
    bool setStartMs(qint64 ms); // clamped to >= 0
    // Trimming the head keeps the remaining audio where it was on the
    // timeline: the start moves with it (clamped at 0).
    bool setTrimIn(qint64 sourceMs);
    bool setTrimOut(qint64 sourceMs);
    bool setGainDb(double db);
    bool setMuted(bool m);
    bool setFades(qint64 inMs, qint64 outMs);
    bool setNoise(const insv::NoiseReduction &nr); // strength clamped to [0, 1]

private:
    void clampFades();
};

// One extra track, under the main video track. Timeline keeps them in three
// groups, top to bottom (Track::Group): the main video's sound (its stream
// tracks), the Audio group (any number of free audio tracks), and the
// Overlay group -- each video (overlay) track, whose clips are drawn as
// insets over the output, followed by its own audio track while it has one.
// Among overlay tracks the first is the top row and draws on top.
struct Track {
    enum class Kind { Audio, Video };
    // What an audio track holds. The main video's sound is on the stream
    // tracks, right under it -- one per kind of .insv audio stream, named by
    // what it is rather than by its order in the file; an overlay's sound is
    // on its overlay track's audio track (`ownerTrackId`); a Free track holds
    // anything (music, voice-over, an unlinked mic).
    enum class Role { Free, CameraMic, ExternalMic, OverlayAudio };
    enum class Group { Main, Audio, Overlay };

    int id = 0; // unique among the tracks
    Kind kind = Kind::Audio;
    Role role = Role::Free; // audio tracks only
    int ownerTrackId = 0;   // OverlayAudio: the overlay track it belongs to
    QString name;
    double gainDb = 0.0; // on top of each clip's own level
    bool muted = false;  // a muted video track is hidden
    QList<TrackClip> clips; // kept sorted by startMs

    Group group() const
    {
        if (kind == Kind::Video || role == Role::OverlayAudio)
            return Group::Overlay;
        return role == Role::Free ? Group::Audio : Group::Main;
    }
    bool isStreamTrack() const { return role == Role::CameraMic || role == Role::ExternalMic; }

    // The role of stream `stream` (InsvAudioDemux's ordinal) of an .insv
    // with `streamCount` audio streams: with an external mic connected the
    // first stream is the external mic (e.g. an Insta360 Mic Air) and the
    // second the camera's own mic, else
    // the only one is the camera's (see docs/formats/insta360-x5.md).
    static Role roleOfStream(int stream, int streamCount)
    {
        if (streamCount >= 2)
            return stream == 0 ? Role::ExternalMic : stream == 1 ? Role::CameraMic : Role::Free;
        return stream == 0 ? Role::CameraMic : Role::Free;
    }

    void sortClips()
    {
        std::stable_sort(clips.begin(), clips.end(),
                         [](const TrackClip &a, const TrackClip &b) { return a.startMs < b.startMs; });
    }
};
