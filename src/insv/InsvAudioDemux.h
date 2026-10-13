// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace insv {

// Sequential decode of an Insta360 `.insv` file's audio stream (AAC on the
// X5). The X5 muxes in *two* AAC streams -- one flagged `default`, one not
// (ffprobe, 2026-09-05) -- initially assumed here to be a throwaway near-
// duplicate, but the user has since identified them as two genuinely
// different sources: the camera's own mic and an external accessory mic
// (Insta360 Mic Air), confirmed by the two streams' differing peak levels
// (ffmpeg volumedetect: -1.3 dB vs -8.9 dB on the sample clip). The caller
// picks which one to decode by its ordinal among the file's audio streams
// (TrackClip::stream: each stream is its own clip on the Camera mic / External mic
// track); the container carries nothing that says which stream is which mic.
//
// Independent of InsvDemux -- its own AVFormatContext, its own file handle --
// because audio playback needs no fisheye video and no stitch path at all.
// Always resamples/remixes to a fixed output format (see open()) so a caller
// never has to handle format variation between clips. FFmpeg-only, no Qt --
// lives in insv_demux next to InsvDemux for the same reason (the one
// FFmpeg-dependent piece of src/insv/).
class InsvAudioDemux {
public:
    InsvAudioDemux() = default;
    ~InsvAudioDemux();
    InsvAudioDemux(const InsvAudioDemux&) = delete;
    InsvAudioDemux& operator=(const InsvAudioDemux&) = delete;

    // Opens `path`'s audio stream and a resampler to interleaved float32 at
    // outSampleRate/outChannels. `track` is the ordinal among the file's audio
    // streams (0 = the first audio stream, whatever its container index);
    // negative or out of range => the `default`-flagged stream (or the first
    // audio stream if none is flagged). Returns false with `err` set if the
    // file has no audio stream at all -- callers should treat that as "play
    // silence for this clip", not necessarily a hard failure.
    bool open(const std::string& path, int outSampleRate, int outChannels, std::string& err,
              int track = -1);

    // Number of audio streams in `path` (0 if none, or the file can't be
    // opened). Cheap-ish: opens the container and reads its stream info.
    static int countAudioStreams(const std::string& path);

    int outSampleRate() const { return m_outRate; }
    int outChannels() const { return m_outChannels; }
    int64_t durationMs() const { return m_durationMs; }

    // Seeks so the next readNext() returns the chunk that contains
    // timestampMs (it starts at or before it, by up to one AAC frame) -- the
    // caller cuts the head off at the exact sample if it needs to (see
    // ClipAudioReader). Returns false with `err` set on failure.
    bool seekTo(int64_t timestampMs, std::string& err);

    // Replaces `out` with the next chunk of interleaved float32 samples (one
    // AAC frame's worth, ~21 ms at 48 kHz) and sets *ptsMs (if non-null) to
    // the chunk's start time, and *ptsSec (if non-null) to the same unrounded,
    // for sample-exact placement. Returns false with `err` empty at
    // end-of-stream, `err` set on a real error. `out` may come back empty (not
    // EOF, not an error) if a conversion produced no samples this call --
    // callers should just call again.
    bool readNext(std::vector<float>& out, int64_t* ptsMs, std::string& err,
                  double* ptsSec = nullptr);

private:
    void close();

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_dec = nullptr;
    SwrContext* m_swr = nullptr;
    AVPacket* m_pkt = nullptr;
    AVFrame* m_frame = nullptr;
    int m_streamIndex = -1;
    int m_outRate = 0;
    int m_outChannels = 0;
    int64_t m_durationMs = 0;
    double m_seekTargetSec = -1.0;
    bool m_flushSent = false;
};

} // namespace insv
