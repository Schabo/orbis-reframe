// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvAudioDemux.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace insv {

InsvAudioDemux::~InsvAudioDemux() { close(); }

void InsvAudioDemux::close()
{
    if (m_swr)
        swr_free(&m_swr);
    if (m_dec)
        avcodec_free_context(&m_dec);
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_pkt)
        av_packet_free(&m_pkt);
    if (m_fmt)
        avformat_close_input(&m_fmt);
    m_streamIndex = -1;
    m_durationMs = 0;
    m_seekTargetSec = -1.0;
    m_flushSent = false;
}

int InsvAudioDemux::countAudioStreams(const std::string& path)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        return 0;
    int n = 0;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            n += fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
    }
    avformat_close_input(&fmt);
    return n;
}

bool InsvAudioDemux::open(const std::string& path, int outSampleRate, int outChannels,
                          std::string& err, int track)
{
    close();
    m_outRate = outSampleRate;
    m_outChannels = outChannels;

    // The caller names the file: it knows which clip that is.
    if (const int rc = avformat_open_input(&m_fmt, path.c_str(), nullptr, nullptr); rc < 0) {
        char why[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(rc, why, sizeof(why));
        err = std::string("avformat_open_input failed: ") + why;
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        err = "avformat_find_stream_info failed";
        close();
        return false;
    }

    // The requested ordinal if it exists, else the stream flagged `default`,
    // else the first audio stream -- see the class comment.
    int fallback = -1;
    int flagged = -1;
    int ordinal = 0;
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        AVStream* st = m_fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (fallback < 0)
            fallback = static_cast<int>(i);
        if (flagged < 0 && (st->disposition & AV_DISPOSITION_DEFAULT))
            flagged = static_cast<int>(i);
        if (ordinal++ == track)
            m_streamIndex = static_cast<int>(i);
    }
    if (m_streamIndex < 0)
        m_streamIndex = flagged >= 0 ? flagged : fallback;
    if (m_streamIndex < 0) {
        err = "no audio stream";
        close();
        return false;
    }

    AVStream* st = m_fmt->streams[m_streamIndex];
    if (st->duration != AV_NOPTS_VALUE)
        m_durationMs = static_cast<int64_t>(st->duration * av_q2d(st->time_base) * 1000.0);
    else if (m_fmt->duration != AV_NOPTS_VALUE)
        m_durationMs = m_fmt->duration / (AV_TIME_BASE / 1000);

    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        err = "no decoder for audio stream " + std::to_string(m_streamIndex);
        close();
        return false;
    }
    m_dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_dec, st->codecpar);
    if (avcodec_open2(m_dec, codec, nullptr) < 0) {
        err = "avcodec_open2 failed for audio stream " + std::to_string(m_streamIndex);
        close();
        return false;
    }

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, m_outChannels);
    AVChannelLayout inLayout = m_dec->ch_layout;
    if (inLayout.nb_channels <= 0)
        av_channel_layout_default(&inLayout, 1);

    if (swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, m_outRate, &inLayout,
                            m_dec->sample_fmt, m_dec->sample_rate, 0, nullptr) < 0 ||
        swr_init(m_swr) < 0) {
        err = "swresample setup failed";
        close();
        return false;
    }

    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    return true;
}

bool InsvAudioDemux::seekTo(int64_t timestampMs, std::string& err)
{
    if (!m_dec) {
        err = "seekTo before open";
        return false;
    }
    // Seek the audio stream itself (not the default video stream, whose
    // keyframe can land right on the target), kSeekPrerollMs early: AAC's
    // first frame after a decoder flush has no overlap history and decodes
    // wrong, so a few frames are decoded and discarded (readNext) first.
    constexpr int64_t kSeekPrerollMs = 100;
    const int64_t prerollMs = std::max<int64_t>(timestampMs - kSeekPrerollMs, 0);
    const AVStream* st = m_fmt->streams[m_streamIndex];
    const int64_t ts = av_rescale_q(prerollMs, AVRational{1, 1000}, st->time_base);
    if (av_seek_frame(m_fmt, m_streamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        err = "av_seek_frame failed at " + std::to_string(timestampMs) + " ms";
        return false;
    }
    avcodec_flush_buffers(m_dec);
    m_flushSent = false;
    m_seekTargetSec = timestampMs > 0 ? timestampMs / 1000.0 : -1.0;
    return true;
}

bool InsvAudioDemux::readNext(std::vector<float>& out, int64_t* ptsMs, std::string& err,
                              double* ptsSec)
{
    for (;;) {
        int r = avcodec_receive_frame(m_dec, m_frame);
        if (r == AVERROR_EOF) {
            err.clear();
            return false;
        }
        if (r == AVERROR(EAGAIN)) {
            if (av_read_frame(m_fmt, m_pkt) < 0) {
                if (!m_flushSent) {
                    avcodec_send_packet(m_dec, nullptr); // drain, then AVERROR_EOF for good
                    m_flushSent = true;
                }
                continue;
            }
            if (m_pkt->stream_index == m_streamIndex) {
                int s = avcodec_send_packet(m_dec, m_pkt);
                if (s < 0 && s != AVERROR(EAGAIN)) {
                    av_packet_unref(m_pkt);
                    err = "avcodec_send_packet failed";
                    return false;
                }
            }
            av_packet_unref(m_pkt);
            continue;
        }
        if (r < 0) {
            err = "avcodec_receive_frame failed";
            return false;
        }

        const AVRational tb = m_fmt->streams[m_streamIndex]->time_base;
        const int64_t pts = (m_frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                 ? m_frame->best_effort_timestamp
                                 : m_frame->pts;
        const double sec = (pts == AV_NOPTS_VALUE) ? 0.0 : pts * av_q2d(tb);
        const int inRate = m_frame->sample_rate > 0 ? m_frame->sample_rate : m_dec->sample_rate;
        const double endSec = sec + (inRate > 0 ? double(m_frame->nb_samples) / inRate : 0.0);
        if (m_seekTargetSec >= 0 && endSec <= m_seekTargetSec + 1e-9) {
            av_frame_unref(m_frame);
            continue; // ends before the seek target -- keep decoding forward
        }
        m_seekTargetSec = -1.0;

        const int cap = std::max(swr_get_out_samples(m_swr, m_frame->nb_samples), 0);
        out.assign(static_cast<size_t>(cap) * m_outChannels, 0.0f);
        uint8_t* outPtr = reinterpret_cast<uint8_t*>(out.data());
        const int produced =
            swr_convert(m_swr, &outPtr, cap, m_frame->extended_data, m_frame->nb_samples);
        av_frame_unref(m_frame);
        if (produced < 0) {
            err = "swr_convert failed";
            return false;
        }
        out.resize(static_cast<size_t>(produced) * m_outChannels);
        if (ptsMs)
            *ptsMs = static_cast<int64_t>(std::llround(sec * 1000.0));
        if (ptsSec)
            *ptsSec = sec;
        return true;
    }
}

} // namespace insv
