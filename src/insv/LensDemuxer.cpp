// SPDX-License-Identifier: MIT OR Apache-2.0
#include "LensDemuxer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

namespace insv {

namespace {
// A seek keeps the first frame within this of the target: targets are whole
// ms, frame times aren't, and a keyframe target (nearestKeyframeMs, rounded up)
// must not drop the keyframe itself.
constexpr double kSeekSlackSec = 0.001;

double ptsSec(const AVFrame* f, AVRational tb)
{
    const int64_t p = (f->best_effort_timestamp == AV_NOPTS_VALUE) ? f->pts : f->best_effort_timestamp;
    return (p == AV_NOPTS_VALUE) ? 0.0 : p * av_q2d(tb);
}

// ptsSec in microseconds, rounded from the time base exactly.
int64_t ptsUs(const AVFrame* f, AVRational tb)
{
    const int64_t p = (f->best_effort_timestamp == AV_NOPTS_VALUE) ? f->pts : f->best_effort_timestamp;
    return (p == AV_NOPTS_VALUE) ? 0 : av_rescale_q(p, tb, AVRational{1, 1000000});
}

// The stream's matrix and range, as its VUI says. Where it doesn't: limited
// range, the HEVC / H.264 default when video_full_range_flag is absent, and
// BT.709, the HD matrix (every lens stream is HD). Logged, as is a matrix the
// stitch can't do.
YcbcrCoding StreamYcbcr(const AVCodecParameters* p, int index)
{
    YcbcrCoding c;
    const auto pf = static_cast<AVPixelFormat>(p->format);
    // yuvj* formats are full range by definition, whatever color_range says.
    if (p->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P ||
        pf == AV_PIX_FMT_YUVJ422P || pf == AV_PIX_FMT_YUVJ444P)
        c.fullRange = true;
    else if (p->color_range == AVCOL_RANGE_MPEG)
        c.fullRange = false;
    else {
        c.fullRange = false;
        std::fprintf(stderr, "[insv] video stream %d doesn't say its range; taking limited\n",
                     index);
    }

    switch (p->color_space) {
    case AVCOL_SPC_BT709: c.matrix = YcbcrMatrix::Bt709; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: c.matrix = YcbcrMatrix::Bt601; break;
    case AVCOL_SPC_BT2020_NCL: c.matrix = YcbcrMatrix::Bt2020; break;
    case AVCOL_SPC_BT2020_CL:
        c.matrix = YcbcrMatrix::Bt2020;
        std::fprintf(stderr, "[insv] video stream %d is BT.2020 constant luminance; decoding it "
                             "as non-constant\n", index);
        break;
    default: {
        c.matrix = YcbcrMatrix::Bt709;
        const char* name = av_color_space_name(p->color_space);
        std::fprintf(stderr, "[insv] video stream %d: %s matrix; taking BT.709\n", index,
                     p->color_space == AVCOL_SPC_UNSPECIFIED ? "no" : name ? name : "unknown");
        break;
    }
    }
    return c;
}
} // namespace

void KeyframesAroundMs(AVFormatContext* fmt, int stream, int64_t timestampMs, int64_t& before,
                       int64_t& after)
{
    before = after = -1;
    if (!fmt || stream < 0 || stream >= static_cast<int>(fmt->nb_streams))
        return;
    AVStream* st = fmt->streams[stream];
    const int64_t ts = av_rescale_q(timestampMs, AVRational{1, 1000}, st->time_base);
    auto ms = [&](const AVIndexEntry* e) {
        return e ? static_cast<int64_t>(std::ceil(e->timestamp * av_q2d(st->time_base) * 1000.0 - 1e-9))
                 : int64_t(-1);
    };
    before = ms(avformat_index_get_entry_from_timestamp(st, ts, AVSEEK_FLAG_BACKWARD));
    after = ms(avformat_index_get_entry_from_timestamp(st, ts, 0));
}

int64_t NearestKeyframeMs(AVFormatContext* fmt, int stream, int64_t timestampMs)
{
    if (!fmt || stream < 0 || stream >= static_cast<int>(fmt->nb_streams))
        return timestampMs;
    AVStream* st = fmt->streams[stream];
    const int64_t ts = av_rescale_q(timestampMs, AVRational{1, 1000}, st->time_base);
    // The keyframe at-or-before and the one after; keyframes only (no
    // AVSEEK_FLAG_ANY).
    const AVIndexEntry* before = avformat_index_get_entry_from_timestamp(st, ts, AVSEEK_FLAG_BACKWARD);
    const AVIndexEntry* after = avformat_index_get_entry_from_timestamp(st, ts, 0);
    const AVIndexEntry* best = before;
    if (!best || (after && after->timestamp - ts < ts - before->timestamp))
        best = after;
    if (!best)
        return timestampMs;
    const double ms = best->timestamp * av_q2d(st->time_base) * 1000.0;
    return static_cast<int64_t>(std::ceil(ms - 1e-9));
}

double KeyframeAtOrBeforeSec(AVFormatContext* fmt, int stream, int64_t timestampMs)
{
    if (!fmt || stream < 0 || stream >= static_cast<int>(fmt->nb_streams))
        return -1.0;
    AVStream* st = fmt->streams[stream];
    const int64_t ts = av_rescale_q(timestampMs, AVRational{1, 1000}, st->time_base);
    const AVIndexEntry* e = avformat_index_get_entry_from_timestamp(st, ts, AVSEEK_FLAG_BACKWARD);
    return e ? e->timestamp * av_q2d(st->time_base) : -1.0;
}

LensDemuxer::~LensDemuxer() { close(); }

void LensDemuxer::clearFifos()
{
    for (auto& q : m_fifo) {
        while (!q.empty()) {
            AVFrame* f = q.front();
            av_frame_free(&f);
            q.pop_front();
        }
    }
}

void LensDemuxer::close()
{
    clearFifos();
    for (auto*& d : m_dec)
        avcodec_free_context(&d);
    m_dec.clear();
    m_fifo.clear();
    m_streamEof.clear();
    if (m_pkt)
        av_packet_free(&m_pkt);
    for (Input& in : m_inputs)
        avformat_close_input(&in.fmt);
    m_inputs.clear();
    m_parts.clear();
    m_part = 0;
    m_lensInput.clear();
    m_videoStreamIndex.clear();
    m_lenses.clear();
    m_seekTargetSec = -1.0;
    m_lastReadSec = -1.0;
}

int LensDemuxer::openInput(const std::string& path, std::string& err)
{
    for (size_t i = 0; i < m_inputs.size(); ++i)
        if (m_inputs[i].path == path)
            return static_cast<int>(i);
    Input in;
    in.path = path;
    // The caller names the file: it knows which clip that is.
    if (const int rc = avformat_open_input(&in.fmt, path.c_str(), nullptr, nullptr); rc < 0) {
        char why[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(rc, why, sizeof(why));
        err = std::string("avformat_open_input failed: ") + why;
        return -1;
    }
    if (avformat_find_stream_info(in.fmt, nullptr) < 0) {
        avformat_close_input(&in.fmt);
        err = "avformat_find_stream_info failed";
        return -1;
    }
    m_inputs.push_back(std::move(in));
    return static_cast<int>(m_inputs.size()) - 1;
}

bool LensDemuxer::addLens(int input, int streamIndex, const DecoderSetup& setup, std::string& err)
{
    AVStream* st = m_inputs[input].fmt->streams[streamIndex];
    InsvStreamInfo info;
    info.width = st->codecpar->width;
    if (const AVPixFmtDescriptor* d =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(st->codecpar->format)))
        info.bitDepth = d->comp[0].depth;
    info.ycbcr = StreamYcbcr(st->codecpar, streamIndex);
    info.height = st->codecpar->height;
    const AVRational fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) {
        err = "no frame rate in video stream " + std::to_string(streamIndex);
        return false;
    }
    info.fps = av_q2d(fr);
    const AVFormatContext* fmt = m_inputs[input].fmt;
    if (st->duration != AV_NOPTS_VALUE) {
        info.durationMs = static_cast<int64_t>(st->duration * av_q2d(st->time_base) * 1000.0);
        info.durationUs = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000000});
    } else if (fmt->duration != AV_NOPTS_VALUE) {
        info.durationMs = fmt->duration / (AV_TIME_BASE / 1000);
        info.durationUs = fmt->duration;
    }

    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        err = "no decoder for video stream " + std::to_string(streamIndex);
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (setup && !setup(st, dec, ctx, err)) {
        avcodec_free_context(&ctx);
        return false;
    }
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        err = "avcodec_open2 failed for video stream " + std::to_string(streamIndex);
        return false;
    }
    m_lensInput.push_back(input);
    m_videoStreamIndex.push_back(streamIndex);
    m_lenses.push_back(info);
    m_dec.push_back(ctx);
    return true;
}

bool LensDemuxer::finishOpen(std::string& err)
{
    if (m_dec.empty()) {
        err = "no video streams";
        close();
        return false;
    }
    m_fifo.resize(m_dec.size());
    m_streamEof.assign(m_dec.size(), 0);
    m_pkt = av_packet_alloc();
    return true;
}

bool LensDemuxer::open(const std::string& path, const DecoderSetup& setup, std::string& err)
{
    close();
    if (openInput(path, err) < 0)
        return false;
    const AVFormatContext* fmt = m_inputs[0].fmt;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        if (!addLens(0, static_cast<int>(i), setup, err)) {
            close();
            return false;
        }
    }
    m_parts.push_back({0, m_lensInput, m_videoStreamIndex,
                       std::vector<int64_t>(m_lensInput.size(), 0)});
    return finishOpen(err);
}

bool LensDemuxer::open(const std::vector<LensSource>& lenses, const DecoderSetup& setup,
                       std::string& err)
{
    return open(std::vector<ClipPart>{{0, lenses}}, setup, err);
}

bool LensDemuxer::openPart(const std::vector<LensSource>& lenses, const DecoderSetup* setup,
                           Part& part, std::string& failedPath, std::string& err)
{
    if (!setup && lenses.size() != m_dec.size()) {
        err = "a part has " + std::to_string(lenses.size()) + " lenses, the first " +
              std::to_string(m_dec.size());
        return false;
    }
    for (size_t l = 0; l < lenses.size(); ++l) {
        const LensSource& src = lenses[l];
        failedPath = src.path;
        const int input = openInput(src.path, err);
        if (input < 0)
            return false;
        // The n-th video stream of the file.
        const AVFormatContext* fmt = m_inputs[input].fmt;
        int streamIndex = -1;
        for (unsigned i = 0, n = 0; i < fmt->nb_streams && streamIndex < 0; ++i)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                static_cast<int>(n++) == src.stream)
                streamIndex = static_cast<int>(i);
        if (streamIndex < 0) {
            err = "no video stream " + std::to_string(src.stream);
            return false;
        }
        if (setup) {
            if (!addLens(input, streamIndex, *setup, err))
                return false;
        } else {
            // The first part's decoders go on into this one: it must be
            // coded alike.
            const AVStream* a = lensStream(static_cast<int>(l));
            const AVStream* b = fmt->streams[streamIndex];
            const AVCodecParameters* pa = a->codecpar;
            const AVCodecParameters* pb = b->codecpar;
            if (pa->codec_id != pb->codec_id || pa->width != pb->width ||
                pa->height != pb->height || pa->format != pb->format ||
                av_cmp_q(a->time_base, b->time_base) != 0 ||
                pa->extradata_size != pb->extradata_size ||
                (pa->extradata_size > 0 &&
                 std::memcmp(pa->extradata, pb->extradata, pa->extradata_size) != 0)) {
                err = "lens " + std::to_string(l) + " isn't coded like the first part's";
                return false;
            }
        }
        part.lensInput.push_back(input);
        part.lensStream.push_back(streamIndex);
    }
    return true;
}

bool LensDemuxer::open(const std::vector<ClipPart>& parts, const DecoderSetup& setup,
                       std::string& err)
{
    close();
    if (parts.empty() || parts.front().lenses.empty()) {
        err = "no lenses to open";
        return false;
    }
    for (size_t p = 0; p < parts.size(); ++p) {
        // The first part's first file is the clip's, which the caller
        // names; another one is named here.
        Part part;
        part.startUs = parts[p].startUs;
        std::string failedPath;
        if (!openPart(parts[p].lenses, p == 0 ? &setup : nullptr, part, failedPath, err)) {
            if (p > 0)
                err += " (part " + std::to_string(p + 1) + ")";
            if (failedPath != parts[0].lenses[0].path)
                err += ": " + failedPath;
            close();
            return false;
        }
        if (p == 0) {
            m_lensInput = part.lensInput;
            m_videoStreamIndex = part.lensStream;
        }
        for (size_t l = 0; l < part.lensInput.size(); ++l)
            part.offset.push_back(av_rescale_q(part.startUs, AVRational{1, 1000000},
                                               lensStream(static_cast<int>(l))->time_base));
        m_parts.push_back(std::move(part));
    }
    // A clip in parts is as long as its last part's end.
    if (m_parts.size() > 1)
        for (size_t l = 0; l < m_lenses.size(); ++l) {
            const Part& last = m_parts.back();
            const AVFormatContext* fmt = m_inputs[last.lensInput[l]].fmt;
            const AVStream* st = fmt->streams[last.lensStream[l]];
            int64_t us = 0;
            if (st->duration != AV_NOPTS_VALUE)
                us = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000000});
            else if (fmt->duration != AV_NOPTS_VALUE)
                us = fmt->duration;
            m_lenses[l].durationUs = last.startUs + us;
            m_lenses[l].durationMs = m_lenses[l].durationUs / 1000;
        }
    return finishOpen(err);
}

AVStream* LensDemuxer::lensStream(int lens) const
{
    return m_inputs[m_lensInput[lens]].fmt->streams[m_videoStreamIndex[lens]];
}

int LensDemuxer::streamToLens(int input, int streamIndex) const
{
    for (size_t l = 0; l < m_videoStreamIndex.size(); ++l)
        if (m_lensInput[l] == input && m_videoStreamIndex[l] == streamIndex)
            return static_cast<int>(l);
    return -1;
}

void LensDemuxer::drainDecoder(int lens)
{
    for (;;) {
        AVFrame* frm = av_frame_alloc();
        const int r = avcodec_receive_frame(m_dec[lens], frm);
        if (r < 0) { // EAGAIN, EOF or an error
            av_frame_free(&frm);
            if (r == AVERROR_EOF)
                m_streamEof[lens] = 1;
            return;
        }
        // Into clip time: a later part's frames count from its own start.
        if (const int64_t off = m_parts[m_part].offset[lens]; off != 0) {
            if (frm->pts != AV_NOPTS_VALUE)
                frm->pts += off;
            if (frm->best_effort_timestamp != AV_NOPTS_VALUE)
                frm->best_effort_timestamp += off;
        }
        m_fifo[lens].push_back(frm);
    }
}

bool LensDemuxer::pumpOnce(int input, std::string& err)
{
    Input& in = m_inputs[input];
    const auto drainAll = [&] {
        for (size_t l = 0; l < m_dec.size(); ++l)
            if (m_lensInput[l] == input)
                drainDecoder(static_cast<int>(l));
    };
    if (in.drainStarted) {
        drainAll();
        return true;
    }
    int r = av_read_frame(in.fmt, m_pkt);
    if (r < 0) {
        for (size_t l = 0; l < m_dec.size(); ++l)
            if (m_lensInput[l] == input)
                avcodec_send_packet(m_dec[l], nullptr);
        in.drainStarted = true;
        drainAll();
        return true;
    }
    const int lens = streamToLens(input, m_pkt->stream_index);
    if (lens >= 0) {
        int s = avcodec_send_packet(m_dec[lens], m_pkt);
        if (s == AVERROR(EAGAIN)) {
            drainDecoder(lens); // decoder full -- make room, then retry once
            s = avcodec_send_packet(m_dec[lens], m_pkt);
        }
        if (s < 0 && s != AVERROR(EAGAIN)) {
            av_packet_unref(m_pkt);
            err = "avcodec_send_packet failed on lens " + std::to_string(lens);
            return false;
        }
        drainDecoder(lens);
    }
    av_packet_unref(m_pkt);
    return true;
}

void LensDemuxer::resetDecoders()
{
    for (auto* d : m_dec)
        avcodec_flush_buffers(d);
    clearFifos();
    for (Input& in : m_inputs)
        in.drainStarted = false;
    std::fill(m_streamEof.begin(), m_streamEof.end(), 0);
}

size_t LensDemuxer::partAt(int64_t timestampMs) const
{
    size_t p = 0;
    while (p + 1 < m_parts.size() && m_parts[p + 1].startUs <= timestampMs * 1000)
        ++p;
    return p;
}

void LensDemuxer::usePart(size_t part)
{
    m_part = part;
    m_lensInput = m_parts[part].lensInput;
    m_videoStreamIndex = m_parts[part].lensStream;
}

void LensDemuxer::partKeyframes(size_t part, int64_t timestampMs, int64_t& before,
                                int64_t& after) const
{
    const Part& pt = m_parts[part];
    AVStream* st = m_inputs[pt.lensInput[0]].fmt->streams[pt.lensStream[0]];
    const int64_t ts = av_rescale_q(timestampMs, AVRational{1, 1000}, st->time_base) - pt.offset[0];
    // Keyframes only (no AVSEEK_FLAG_ANY).
    const AVIndexEntry* b = avformat_index_get_entry_from_timestamp(st, ts, AVSEEK_FLAG_BACKWARD);
    const AVIndexEntry* a = avformat_index_get_entry_from_timestamp(st, ts, 0);
    before = b ? b->timestamp + pt.offset[0] : kNoKeyframe;
    after = a ? a->timestamp + pt.offset[0] : kNoKeyframe;
}

void LensDemuxer::keyframesAround(int64_t timestampMs, int64_t& before, int64_t& after) const
{
    const size_t p = partAt(timestampMs);
    partKeyframes(p, timestampMs, before, after);
    int64_t unused = 0;
    if (after == kNoKeyframe && p + 1 < m_parts.size())
        partKeyframes(p + 1, m_parts[p + 1].startUs / 1000, unused, after);
    if (before == kNoKeyframe && p > 0)
        partKeyframes(p - 1, m_parts[p].startUs / 1000 - 1, before, unused);
}

void LensDemuxer::keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const
{
    if (m_dec.empty()) {
        before = after = -1;
        return;
    }
    if (m_parts.size() == 1) {
        KeyframesAroundMs(m_inputs[m_lensInput[0]].fmt, m_videoStreamIndex[0], timestampMs,
                          before, after);
        return;
    }
    int64_t b = 0, a = 0;
    keyframesAround(timestampMs, b, a);
    const AVRational tb = lensStream(0)->time_base;
    const auto ms = [&](int64_t t) {
        return t == kNoKeyframe
                   ? int64_t(-1)
                   : static_cast<int64_t>(std::ceil(t * av_q2d(tb) * 1000.0 - 1e-9));
    };
    before = ms(b);
    after = ms(a);
}

int64_t LensDemuxer::nearestKeyframeMs(int64_t timestampMs) const
{
    if (m_dec.empty())
        return timestampMs;
    if (m_parts.size() == 1)
        return NearestKeyframeMs(m_inputs[m_lensInput[0]].fmt, m_videoStreamIndex[0],
                                 timestampMs);
    int64_t before = 0, after = 0;
    keyframesAround(timestampMs, before, after);
    const AVRational tb = lensStream(0)->time_base;
    const int64_t ts = av_rescale_q(timestampMs, AVRational{1, 1000}, tb);
    int64_t best = before;
    if (best == kNoKeyframe || (after != kNoKeyframe && after - ts < ts - before))
        best = after;
    if (best == kNoKeyframe)
        return timestampMs;
    return static_cast<int64_t>(std::ceil(best * av_q2d(tb) * 1000.0 - 1e-9));
}

bool LensDemuxer::seekTo(int64_t timestampMs, std::string& err)
{
    if (m_dec.empty()) {
        err = "seekTo before open";
        return false;
    }
    // Ahead of the last frame read with no keyframe in between: the seek would
    // land on the keyframe this GOP started from and decode more frames than
    // carrying on from here (frame steps, short forward scrubs). Lens 0's
    // keyframes decide; with a file per lens, carrying on is right for the
    // other one too, just not always the cheaper way.
    // Only within the part being read: another part's files start over.
    const double targetSec = timestampMs / 1000.0;
    const size_t part = partAt(timestampMs);
    if (m_lastReadSec >= 0 && targetSec > m_lastReadSec + kSeekSlackSec && part == m_part) {
        double kf = -1.0;
        if (m_parts.size() == 1) {
            kf = KeyframeAtOrBeforeSec(m_inputs[m_lensInput[0]].fmt, m_videoStreamIndex[0],
                                       timestampMs);
        } else {
            int64_t before = 0, unused = 0;
            partKeyframes(part, timestampMs, before, unused);
            if (before != kNoKeyframe)
                kf = before * av_q2d(lensStream(0)->time_base);
        }
        if (kf >= 0 && kf <= m_lastReadSec + kSeekSlackSec) {
            m_seekTargetSec = targetSec;
            return true;
        }
    }
    m_lastReadSec = -1.0;
    usePart(part);
    // In the part's own time.
    const int64_t targetUs = std::max<int64_t>(
        0, timestampMs * (AV_TIME_BASE / 1000) - m_parts[part].startUs);
    for (size_t i = 0; i < m_inputs.size(); ++i) {
        const std::vector<int>& ins = m_parts[part].lensInput;
        if (std::find(ins.begin(), ins.end(), static_cast<int>(i)) == ins.end())
            continue;
        Input& in = m_inputs[i];
        if (av_seek_frame(in.fmt, -1, targetUs, AVSEEK_FLAG_BACKWARD) < 0) {
            err = "av_seek_frame failed at " + std::to_string(timestampMs) + " ms";
            if (in.path != m_inputs[m_parts[0].lensInput[0]].path)
                err += ": " + in.path;
            return false;
        }
    }
    resetDecoders();
    // Frame-exact from 1 ms on (frame stepping reaches frame 1 at ~34 ms).
    m_seekTargetSec = timestampMs > 0 ? timestampMs / 1000.0 : -1.0;
    return true;
}

bool LensDemuxer::nextPart(std::string& err)
{
    err.clear();
    if (m_part + 1 >= m_parts.size())
        return false;
    // Every lens of this part to its end, so the decoders can start over;
    // a lens's frames left without a partner (a file a frame longer than
    // the other) go with it.
    for (size_t l = 0; l < m_dec.size(); ++l)
        while (!m_streamEof[l])
            if (!pumpOnce(m_lensInput[l], err))
                return false;
    usePart(m_part + 1);
    for (const int i : m_lensInput) {
        Input& in = m_inputs[i];
        if (av_seek_frame(in.fmt, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
            err = "av_seek_frame failed at the start of " + in.path;
            return false;
        }
    }
    resetDecoders();
    return true;
}

void LensDemuxer::dropStale(int lens)
{
    const AVRational tb = lensStream(lens)->time_base;
    while (!m_fifo[lens].empty() && m_seekTargetSec >= 0 &&
           ptsSec(m_fifo[lens].front(), tb) + kSeekSlackSec < m_seekTargetSec) {
        AVFrame* f = popFront(lens);
        av_frame_free(&f);
    }
}

AVFrame* LensDemuxer::popFront(int lens)
{
    AVFrame* f = m_fifo[lens].front();
    m_fifo[lens].pop_front();
    return f;
}

bool LensDemuxer::readNextPair(AVFrame*& out0, AVFrame*& out1, FramePts* pts, std::string& err)
{
    if (m_dec.size() < 2) {
        err = "readNextPair requires 2 lens streams, have " + std::to_string(m_dec.size());
        return false;
    }
    for (;;) {
        dropStale(0);
        dropStale(1);
        if (!m_fifo[0].empty() && !m_fifo[1].empty())
            break;
        const bool ex0 = m_streamEof[0] && m_fifo[0].empty();
        const bool ex1 = m_streamEof[1] && m_fifo[1].empty();
        if (ex0 || ex1) {
            if (nextPart(err))
                continue;
            return false; // `err` empty: a clean end of stream
        }
        // The file of a lens still waiting for its frame (the same one for
        // both lenses of one file).
        if (!pumpOnce(m_lensInput[m_fifo[0].empty() ? 0 : 1], err))
            return false;
    }
    out0 = popFront(0);
    out1 = popFront(1);
    m_seekTargetSec = -1.0; // satisfied by this pair
    m_lastReadSec = ptsSec(out0, lensStream(0)->time_base);
    if (pts) {
        pts->ms = static_cast<int64_t>(std::llround(m_lastReadSec * 1000.0));
        pts->us = ptsUs(out0, lensStream(0)->time_base);
    }
    return true;
}

bool LensDemuxer::readNextFrame(int lens, AVFrame*& out, std::string& err)
{
    if (lens < 0 || lens >= static_cast<int>(m_dec.size())) {
        err = "lens index " + std::to_string(lens) + " out of range";
        return false;
    }
    for (;;) {
        dropStale(lens);
        if (!m_fifo[lens].empty())
            break;
        if (m_streamEof[lens]) {
            if (nextPart(err))
                continue;
            return false; // `err` empty: a clean end of stream
        }
        if (!pumpOnce(m_lensInput[lens], err))
            return false;
    }
    out = popFront(lens);
    m_seekTargetSec = -1.0;
    // The shortcut in seekTo reads lens 0's keyframes.
    m_lastReadSec = lens == 0 ? ptsSec(out, lensStream(0)->time_base) : -1.0;
    return true;
}

AVFrame* LensDemuxer::peekFrame(int lens, std::string* err)
{
    if (lens < 0 || lens >= static_cast<int>(m_dec.size()))
        return nullptr;
    std::string e;
    for (;;) {
        dropStale(lens);
        if (!m_fifo[lens].empty())
            return m_fifo[lens].front();
        if (m_streamEof[lens] && nextPart(e))
            continue;
        if (m_streamEof[lens] || !e.empty() || !pumpOnce(m_lensInput[lens], e)) {
            if (err)
                *err = e;
            return nullptr;
        }
    }
}

} // namespace insv
