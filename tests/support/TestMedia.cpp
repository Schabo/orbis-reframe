#include "TestMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

#include <algorithm>
#include <cmath>
#include <fstream>

namespace testmedia {

void writeWav(const std::string &path, int frames, const std::function<double(int)> &sample)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    const uint32_t dataBytes = static_cast<uint32_t>(frames) * kWavChannels * 2;
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(kWavChannels);
    u32(kWavRate);
    u32(kWavRate * kWavChannels * 2);
    u16(kWavChannels * 2);
    u16(16);
    f.write("data", 4);
    u32(dataBytes);
    for (int i = 0; i < frames; ++i) {
        const int16_t s = static_cast<int16_t>(std::lround(sample(i) * 32768.0));
        for (int c = 0; c < kWavChannels; ++c)
            f.write(reinterpret_cast<const char *>(&s), 2);
    }
}

bool writeVideo(const std::string &path, const VideoSpec &spec)
{
    AVFormatContext *fmt = nullptr;
    avformat_alloc_output_context2(&fmt, nullptr, nullptr, path.c_str());
    if (!fmt)
        return false;
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!enc)
        return false;
    AVCodecContext *ctx = avcodec_alloc_context3(enc);
    ctx->width = spec.width;
    ctx->height = spec.height;
    ctx->time_base = AVRational{1, spec.fps};
    ctx->framerate = AVRational{spec.fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (spec.hlg) {
        ctx->color_trc = AVCOL_TRC_ARIB_STD_B67;
        ctx->color_primaries = AVCOL_PRI_BT2020;
        ctx->colorspace = AVCOL_SPC_BT2020_NCL;
    }
    ctx->gop_size = 10;
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = FF_QP2LAMBDA * 2;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx, enc, nullptr) < 0)
        return false;
    AVStream *st = avformat_new_stream(fmt, nullptr);
    avcodec_parameters_from_context(st->codecpar, ctx);
    st->time_base = ctx->time_base;
    if (spec.rotationCw) {
        AVPacketSideData *sd = av_packet_side_data_new(&st->codecpar->coded_side_data,
                                                       &st->codecpar->nb_coded_side_data,
                                                       AV_PKT_DATA_DISPLAYMATRIX, 9 * 4, 0);
        av_display_rotation_set(reinterpret_cast<int32_t *>(sd->data), spec.rotationCw); // clockwise
    }
    if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(fmt, nullptr) < 0)
        return false;
    AVFrame *f = av_frame_alloc();
    f->format = ctx->pix_fmt;
    f->width = spec.width;
    f->height = spec.height;
    av_frame_get_buffer(f, 0);
    AVPacket *pkt = av_packet_alloc();
    auto drain = [&](AVFrame *in) {
        avcodec_send_frame(ctx, in);
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            // A frame long: without it the last frame gets no duration and
            // falls outside the MP4's edit list, so it never decodes.
            pkt->duration = 1;
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
        }
    };
    for (int i = 0; i < spec.frames; ++i) {
        av_frame_make_writable(f);
        for (int y = 0; y < spec.height; ++y)
            for (int x = 0; x < spec.width; ++x)
                f->data[0][y * f->linesize[0] + x] = spec.luma ? spec.luma(i, x, y) : 128;
        for (int y = 0; y < spec.height / 2; ++y)
            for (int x = 0; x < spec.width / 2; ++x) {
                f->data[1][y * f->linesize[1] + x] = 128;
                f->data[2][y * f->linesize[2] + x] = 128;
            }
        f->pts = i;
        drain(f);
    }
    drain(nullptr);
    av_write_trailer(fmt);
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&ctx);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return true;
}

std::vector<uint8_t> encodeH264Picture(int width, int height, uint8_t luma)
{
    std::vector<uint8_t> out;
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!enc)
        return out;
    AVCodecContext *ctx = avcodec_alloc_context3(enc);
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational{1, 25};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->gop_size = 1;
    ctx->max_b_frames = 0;
    if (avcodec_open2(ctx, enc, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return out;
    }
    AVFrame *f = av_frame_alloc();
    f->format = ctx->pix_fmt;
    f->width = width;
    f->height = height;
    av_frame_get_buffer(f, 0);
    for (int y = 0; y < height; ++y)
        std::fill_n(f->data[0] + y * f->linesize[0], width, luma);
    for (int p = 1; p < 3; ++p)
        for (int y = 0; y < height / 2; ++y)
            std::fill_n(f->data[p] + y * f->linesize[p], width / 2, uint8_t(128));
    f->pts = 0;
    AVPacket *pkt = av_packet_alloc();
    for (AVFrame *in : {f, static_cast<AVFrame *>(nullptr)}) {
        avcodec_send_frame(ctx, in);
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            out.insert(out.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&ctx);
    return out;
}

} // namespace testmedia
