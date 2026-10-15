// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

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
// way. FFmpeg-only, no Qt.
class ClipAudioReader {
public:
    ClipAudioReader(int sampleRate, int channels);
    ~ClipAudioReader();
    ClipAudioReader(const ClipAudioReader&) = delete;
    ClipAudioReader& operator=(const ClipAudioReader&) = delete;

    // Opens audio stream `track` (InsvAudioDemux::open's ordinal) of `path`.
    // False if the file has no audio -- the reader then just yields silence.
    // Leaves the position at source frame 0.
    bool open(const std::string& path, int track);
    bool hasAudio() const { return m_demux != nullptr; }

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
    struct Tempo;                                 // libavfilter atempo graph
    std::unique_ptr<Tempo> m_tempo;               // null at tempo 1
    double m_speed = 1.0;
    const int m_rate;
    const int m_channels;
    std::unique_ptr<InsvAudioDemux> m_demux;
    bool m_muted = false;
    bool m_eof = false;
    bool m_seekPending = true; // demux not yet positioned at m_pos
    int64_t m_pos = 0;         // next source frame read() returns
    NoiseReduction m_noise;
    std::unique_ptr<NoiseFilter> m_filter; // for m_noise.mode; null = no filtering
    bool m_filterPrimed = false;
    int64_t m_outPos = 0; // while filtering: source frame of the next output
    std::vector<float> m_filterIn;
    std::vector<float> m_chunk;
    int64_t m_chunkStart = 0; // source frame of m_chunk's first sample
};

} // namespace insv
