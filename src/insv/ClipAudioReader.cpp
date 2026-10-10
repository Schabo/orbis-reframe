// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ClipAudioReader.h"

#include "InsvAudioDemux.h"
#include "NoiseFilter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

namespace insv {

// abuffer -> atempo[,atempo...] -> aformat(flt) -> abuffersink, interleaved
// float32 in and out. atempo takes 0.5..100 per stage, so slower speeds chain
// 0.5 stages (0.25 = 0.5 x 0.5).
struct ClipAudioReader::Tempo {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    AVFrame* frame = nullptr;
    std::deque<float> out; // stretched frames not handed out yet
    int64_t pts = 0;       // input sample count, for the abuffer timestamps

    ~Tempo()
    {
        av_frame_free(&frame);
        avfilter_graph_free(&graph);
    }

    bool init(double speed, int rate, int channels)
    {
        graph = avfilter_graph_alloc();
        frame = av_frame_alloc();
        if (!graph || !frame)
            return false;
        AVChannelLayout layout;
        av_channel_layout_default(&layout, channels);
        char layoutName[64] = {};
        av_channel_layout_describe(&layout, layoutName, sizeof layoutName);
        av_channel_layout_uninit(&layout);
        const std::string srcArgs = "sample_rate=" + std::to_string(rate) +
                                    ":sample_fmt=flt:channel_layout=" + layoutName +
                                    ":time_base=1/" + std::to_string(rate);
        if (avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), "in",
                                         srcArgs.c_str(), nullptr, graph) < 0 ||
            avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out",
                                         nullptr, nullptr, graph) < 0)
            return false;

        std::string chain;
        double s = speed;
        while (s < 0.5 - 1e-9) {
            chain += "atempo=0.5,";
            s /= 0.5;
        }
        // to_chars, not to_string: a Qt app runs under the user's LC_NUMERIC,
        // which may use a decimal comma ("0,5" breaks the filter string).
        char num[32];
        const auto r = std::to_chars(num, num + sizeof num, s);
        chain += "atempo=" + std::string(num, r.ptr) + ",aformat=sample_fmts=flt:sample_rates=" +
                 std::to_string(rate) + ":channel_layouts=" + layoutName;

        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = src;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink;
        const int ret = avfilter_graph_parse_ptr(graph, chain.c_str(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        return ret >= 0 && avfilter_graph_config(graph, nullptr) >= 0;
    }

    // Push `n` interleaved frames, then collect whatever the graph has ready.
    bool push(const float* pcm, int n, int rate, int channels)
    {
        av_frame_unref(frame);
        frame->format = AV_SAMPLE_FMT_FLT;
        frame->sample_rate = rate;
        av_channel_layout_default(&frame->ch_layout, channels);
        frame->nb_samples = n;
        frame->pts = pts;
        if (av_frame_get_buffer(frame, 0) < 0)
            return false;
        std::memcpy(frame->data[0], pcm, static_cast<size_t>(n) * channels * sizeof(float));
        pts += n;
        if (av_buffersrc_add_frame(src, frame) < 0)
            return false;
        while (av_buffersink_get_frame(sink, frame) >= 0) {
            const float* d = reinterpret_cast<const float*>(frame->data[0]);
            out.insert(out.end(), d, d + static_cast<size_t>(frame->nb_samples) * channels);
            av_frame_unref(frame);
        }
        return true;
    }
};

ClipAudioReader::ClipAudioReader(int sampleRate, int channels)
    : m_rate(sampleRate), m_channels(channels)
{
}

ClipAudioReader::~ClipAudioReader() = default;

bool ClipAudioReader::open(const std::string& path, int track)
{
    m_demux = std::make_unique<InsvAudioDemux>();
    std::string err;
    if (!m_demux->open(path, m_rate, m_channels, err, track))
        m_demux.reset();
    seek(0);
    return hasAudio();
}

void ClipAudioReader::setGainDb(double db)
{
    m_gain = static_cast<float>(std::pow(10.0, db / 20.0));
}

void ClipAudioReader::setTempo(double speed)
{
    if (!(speed > 0.0) || std::abs(speed - m_speed) < 1e-9)
        return;
    m_speed = speed;
    m_tempo.reset(); // rebuilt on the next read()
}

void ClipAudioReader::setNoiseReduction(const NoiseReduction& nr)
{
    NoiseReduction n = nr;
    n.strength = std::clamp(n.strength, 0.0f, 1.0f);
    if (n == m_noise)
        return;
    const bool modeChanged = n.mode != m_noise.mode;
    const int64_t pos = position();
    m_noise = n;
    if (modeChanged) // null for Off (and a mode this build can't run)
        m_filter = makeNoiseFilter(n.mode, m_rate, m_channels);
    if (m_filter)
        m_filter->setStrength(n.strength);
    if (modeChanged)
        seek(pos); // the other path's buffered audio is for another position
}

void ClipAudioReader::seek(int64_t frame)
{
    m_pos = std::max<int64_t>(frame, 0);
    m_outPos = m_pos;
    m_filterPrimed = false;
    m_seekPending = true;
    m_eof = false;
    m_tempo.reset(); // the stretcher's buffered audio belongs to the old position
}

void ClipAudioReader::read(float* out, int64_t frames)
{
    if (std::abs(m_speed - 1.0) < 1e-9) {
        readFiltered(out, frames);
        return;
    }
    const size_t ch = static_cast<size_t>(m_channels);
    if (!m_tempo) {
        m_tempo = std::make_unique<Tempo>();
        if (!m_tempo->init(m_speed, m_rate, m_channels)) {
            // No stretcher: unstretched audio would drift out of step with the
            // picture, so fall back to silence.
            m_tempo.reset();
            std::memset(out, 0, static_cast<size_t>(frames) * ch * sizeof(float));
            return;
        }
    }
    // Feed the source in small blocks until the stretcher has enough out.
    constexpr int kBlock = 1024;
    std::vector<float> block(static_cast<size_t>(kBlock) * ch);
    const size_t want = static_cast<size_t>(frames) * ch;
    int guard = 0;
    while (m_tempo->out.size() < want && guard++ < 100000) {
        readFiltered(block.data(), kBlock);
        if (!m_tempo->push(block.data(), kBlock, m_rate, m_channels))
            break;
    }
    const size_t n = std::min(want, m_tempo->out.size());
    std::copy_n(m_tempo->out.begin(), n, out);
    m_tempo->out.erase(m_tempo->out.begin(), m_tempo->out.begin() + static_cast<std::ptrdiff_t>(n));
    if (n < want)
        std::memset(out + n, 0, (want - n) * sizeof(float));
}

void ClipAudioReader::readFiltered(float* out, int64_t frames)
{
    if (!filtering()) {
        readSource(out, frames);
        return;
    }
    if (!m_filterPrimed)
        primeFilter();
    const size_t n = static_cast<size_t>(frames) * static_cast<size_t>(m_channels);
    m_filterIn.resize(n);
    readSource(m_filterIn.data(), frames); // m_pos runs latency() ahead of m_outPos
    m_filter->process(m_filterIn.data(), out, static_cast<int>(frames));
    m_outPos += frames;
}

// Line the filter up so its next output is source frame m_outPos: start it
// its warm-up before that (or at the file's start, if that's nearer) and run
// it through to m_outPos plus its latency, throwing that output away. Near
// the file's start, where there's less than the warm-up to learn from, it
// first learns from the opening warm-up's worth and rewinds (keeping what it
// learned), so a clip's first seconds are filtered like the rest.
void ClipAudioReader::primeFilter()
{
    m_filter->setStrength(m_noise.strength);
    m_filter->reset();
    const size_t ch = static_cast<size_t>(m_channels);
    constexpr int64_t kBlock = 4096;
    std::vector<float> in(static_cast<size_t>(kBlock) * ch), out(in.size());
    auto runFrom = [&](int64_t from, int64_t frames) { // output discarded
        m_pos = from;
        m_seekPending = true;
        m_eof = false;
        for (int64_t left = frames; left > 0;) {
            const int64_t n = std::min(left, kBlock);
            readSource(in.data(), n);
            m_filter->process(in.data(), out.data(), static_cast<int>(n));
            left -= n;
        }
    };
    const int64_t warmup = std::llround(m_filter->warmupSeconds() * m_rate);
    if (m_outPos < warmup) {
        runFrom(0, warmup);
        m_filter->rewind();
    }
    const int64_t start = std::max<int64_t>(m_outPos - warmup, 0);
    runFrom(start, m_outPos - start + m_filter->latency());
    m_filterPrimed = true;
}

void ClipAudioReader::readSource(float* out, int64_t frames)
{
    const size_t ch = static_cast<size_t>(m_channels);
    int64_t done = 0;
    auto silence = [&](int64_t n) {
        std::memset(out + done * ch, 0, static_cast<size_t>(n) * ch * sizeof(float));
        done += n;
        m_pos += n;
    };

    while (done < frames) {
        if (!m_demux || m_muted || m_eof) {
            if (m_muted)
                m_seekPending = true; // demux fell behind m_pos -- re-seek on unmute
            silence(frames - done);
            break;
        }
        if (m_seekPending) {
            // Floor to the ms: InsvAudioDemux::seekTo returns the chunk that
            // contains that instant, which starts at or before m_pos.
            std::string err;
            m_seekPending = false;
            m_chunk.clear();
            m_chunkStart = 0; // no stale "hole" up to the old chunk's start
            if (!m_demux->seekTo(m_pos * 1000 / m_rate, err)) {
                m_eof = true;
                continue;
            }
        }
        const int64_t chunkFrames = static_cast<int64_t>(m_chunk.size() / ch);
        if (m_pos >= m_chunkStart && m_pos < m_chunkStart + chunkFrames) {
            const int64_t off = m_pos - m_chunkStart;
            const int64_t n = std::min(frames - done, chunkFrames - off);
            const float* src = m_chunk.data() + off * ch;
            float* dst = out + done * ch;
            for (size_t k = 0; k < static_cast<size_t>(n) * ch; ++k)
                dst[k] = src[k] * m_gain;
            done += n;
            m_pos += n;
        } else if (m_pos < m_chunkStart) {
            silence(std::min(frames - done, m_chunkStart - m_pos)); // a hole in the stream
        } else {
            double sec = 0.0;
            int64_t ms = 0;
            std::string err;
            if (!m_demux->readNext(m_chunk, &ms, err, &sec)) {
                m_eof = true; // end of stream (or a decode error): silence from here
                m_chunk.clear();
                continue;
            }
            m_chunkStart = std::llround(sec * m_rate);
        }
    }
}

} // namespace insv
