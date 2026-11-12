// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "AudioPart.h"
#include "NoiseReduction.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace insv {

class InsvAudioDemux;
class NoiseFilter;

// Sample-exact reads of one clip's audio: "exactly N frames of interleaved
// float32 PCM starting at source frame S", with the clip's level/mute baked
// in. Wraps InsvAudioDemux, whose chunks are whole AAC frames (~21 ms) placed
// by PTS: read() cuts them at the exact sample, and fills silence wherever
// there's no audio to give (no stream, muted, a hole in the stream, past its
// end), so the output is always exactly as long as asked. Shared by playback
// (AudioPlayer) and export (ReframeExportController) so both trim the same
// way. A clip in several files (AudioPart) reads as one stream: each part's
// sound plays from its start up to the next part's, where the next takes
// over with a short crossfade (kJoinFadeMs). FFmpeg-only, no Qt.
class ClipAudioReader {
public:
    ClipAudioReader(int sampleRate, int channels);
    ~ClipAudioReader();
    ClipAudioReader(const ClipAudioReader&) = delete;
    ClipAudioReader& operator=(const ClipAudioReader&) = delete;

    // How long a part's sound fades into the next's. The camera doesn't
    // record across the cut: on the X5 the first file's sound runs a few
    // tens of ms past its last frame, and the next file's starts at its
    // first with a short rise (docs/formats/insta360-x5.md §1). The fade
    // runs from the join over the first part's tail.
    static constexpr int kJoinFadeMs = 20;

    // Opens audio stream `track` (InsvAudioDemux::open's ordinal) of every
    // part, in clip order. False if no part has audio -- the reader then just
    // yields silence (so does a part without). Leaves the position at source
    // frame 0.
    bool open(const std::vector<AudioPart>& parts, int track);
    bool open(const std::string& path, int track) { return open({{path, 0}}, track); }
    bool hasAudio() const;

    void setMuted(bool muted) { m_muted = muted; }
    // Playback speed (ClipItem::speed): read() then returns audio stretched to
    // play at this rate with the pitch unchanged -- each output frame covers
    // `speed` source frames. 1 = off. Uses FFmpeg's atempo (chained below
    // 0.5x). Resets on seek().
    void setTempo(double speed);
    // Noise reduction on the source audio, before the tempo stretch. Output
    // stays sample-aligned: on a seek the filter is run in over its warm-up
    // (NoiseFilter::warmupSeconds, 0.5-2 s) before the position, and its
    // look-ahead read past it. A change
    // of mode re-seeks to the current position; of strength alone, not.
    void setNoiseReduction(const NoiseReduction& nr);

    // The next read() starts at source frame `frame` (at the output rate).
    void seek(int64_t frame);
    int64_t position() const { return filtering() ? m_outPos : m_pos; }

    // Writes exactly `frames` output frames (frames * channels floats) to
    // `out`. The source position advances by that much at tempo 1, by about
    // frames * tempo otherwise (plus the stretcher's lookahead).
    void read(float* out, int64_t frames);

private:
    bool filtering() const { return m_filter != nullptr; }
    void readFiltered(float* out, int64_t frames); // readSource + noise reduction
    void primeFilter();
    void readSource(float* out, int64_t frames); // the tempo-1 read
    // One part's audio stream, placed in clip frames.
    struct Part {
        std::unique_ptr<InsvAudioDemux> demux; // null: no audio in this file
        int64_t start = 0;                     // clip frame of its stream's time 0
        bool eof = false;
        bool seekPending = true; // demux not yet positioned at pos
        int64_t pos = 0;         // clip frame its next read returns
        std::vector<float> chunk;
        int64_t chunkStart = 0; // clip frame of chunk's first sample
    };
    // Part `p`'s sound for clip frames [from, from + frames), silence where it
    // has none.
    void readPart(Part& p, float* out, int64_t from, int64_t frames);
    void repositionParts(); // after a jump of m_pos: each part seeks on its next read
    struct Tempo;                                 // libavfilter atempo graph
    std::unique_ptr<Tempo> m_tempo;               // null at tempo 1
    double m_speed = 1.0;
    const int m_rate;
    const int m_channels;
    std::vector<Part> m_parts; // in clip order
    bool m_muted = false;
    int64_t m_pos = 0; // next source frame read() returns
    NoiseReduction m_noise;
    std::unique_ptr<NoiseFilter> m_filter; // for m_noise.mode; null = no filtering
    bool m_filterPrimed = false;
    int64_t m_outPos = 0; // while filtering: source frame of the next output
    std::vector<float> m_filterIn;
    std::vector<float> m_joinTail; // the part before's sound over a join's fade
};

} // namespace insv
