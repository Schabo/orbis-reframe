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
    const AVCodec *enc = avcodec_find_encoder(spec.tenBit ? AV_CODEC_ID_FFV1
                                              : spec.h264 ? AV_CODEC_ID_H264
                                                          : AV_CODEC_ID_MPEG4);
    if (!enc)
        return false;
    AVCodecContext *ctx = avcodec_alloc_context3(enc);
    ctx->width = spec.width;
    ctx->height = spec.height;
    ctx->time_base = AVRational{spec.fpsDen, spec.fps};
    ctx->framerate = AVRational{spec.fps, spec.fpsDen};
    ctx->pix_fmt = spec.tenBit ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
    if (spec.hlg) {
        ctx->color_trc = AVCOL_TRC_ARIB_STD_B67;
        ctx->color_primaries = AVCOL_PRI_BT2020;
        ctx->colorspace = AVCOL_SPC_BT2020_NCL;
    }
    ctx->gop_size = 10;
    ctx->max_b_frames = 0;
    if (!spec.h264 && !spec.tenBit) { // near-lossless, so a frame's grey reads back as written
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ctx->global_quality = FF_QP2LAMBDA * 2;
    }
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx, enc, nullptr) < 0)
        return false;
    std::vector<AVStream *> streams;
    for (int s = 0; s < spec.streams; ++s) {
        AVStream *st = avformat_new_stream(fmt, nullptr);
        avcodec_parameters_from_context(st->codecpar, ctx);
        st->time_base = ctx->time_base;
        if (spec.rotationCw) {
            AVPacketSideData *sd = av_packet_side_data_new(&st->codecpar->coded_side_data,
                                                           &st->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_DISPLAYMATRIX, 9 * 4, 0);
            av_display_rotation_set(reinterpret_cast<int32_t *>(sd->data), spec.rotationCw); // clockwise
        }
        streams.push_back(st);
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
    AVPacket *copy = av_packet_alloc();
    auto drain = [&](AVFrame *in) {
        avcodec_send_frame(ctx, in);
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            // A frame long: without it the last frame gets no duration and
            // falls outside the MP4's edit list, so it never decodes.
            pkt->duration = 1;
            for (size_t s = 0; s < streams.size(); ++s) {
                AVStream *st = streams[s];
                // No B-frames: a packet's pts is its frame's index.
                if (spec.keep && !spec.keep(static_cast<int>(s), static_cast<int>(pkt->pts)))
                    continue;
                av_packet_ref(copy, pkt);
                av_packet_rescale_ts(copy, ctx->time_base, st->time_base);
                copy->stream_index = st->index;
                av_interleaved_write_frame(fmt, copy);
            }
            av_packet_unref(pkt);
        }
    };
    for (int i = 0; i < spec.frames; ++i) {
        av_frame_make_writable(f);
        // 10-bit: each sample in two bytes, the 8-bit value << 2.
        const auto put = [&](int plane, int x, int y, int v) {
            uint8_t *row = f->data[plane] + y * f->linesize[plane];
            if (spec.tenBit)
                reinterpret_cast<uint16_t *>(row)[x] = static_cast<uint16_t>(v << 2);
            else
                row[x] = static_cast<uint8_t>(v);
        };
        for (int y = 0; y < spec.height; ++y)
            for (int x = 0; x < spec.width; ++x)
                put(0, x, y, spec.luma ? spec.luma(i, x, y) : 128);
        for (int y = 0; y < spec.height / 2; ++y)
            for (int x = 0; x < spec.width / 2; ++x) {
                put(1, x, y, 128);
                put(2, x, y, 128);
            }
        f->pts = i;
        drain(f);
    }
    drain(nullptr);
    av_write_trailer(fmt);
    av_packet_free(&copy);
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

bool writeJpeg(const std::string &path, int width, int height,
               const std::function<uint8_t(int x, int y)> &luma,
               const std::vector<uint8_t> &trailer, size_t metadataBytes)
{
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!enc)
        return false;
    AVCodecContext *ctx = avcodec_alloc_context3(enc);
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational{1, 25};
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->color_range = AVCOL_RANGE_JPEG;
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = FF_QP2LAMBDA * 2; // near lossless: the tests read the greys back
    if (avcodec_open2(ctx, enc, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return false;
    }
    AVFrame *f = av_frame_alloc();
    f->format = ctx->pix_fmt;
    f->width = width;
    f->height = height;
    f->quality = ctx->global_quality;
    av_frame_get_buffer(f, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            f->data[0][y * f->linesize[0] + x] = luma(x, y);
    for (int p = 1; p < 3; ++p)
        for (int y = 0; y < height / 2; ++y)
            std::fill_n(f->data[p] + y * f->linesize[p], width / 2, uint8_t(128));
    f->pts = 0;
    std::vector<uint8_t> out;
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
    if (out.size() < 2)
        return false;
    // APP9 segments (no camera's) after the start marker, each at most a
    // segment's length.
    std::vector<uint8_t> metadata;
    for (size_t left = metadataBytes; left > 0;) {
        const size_t n = std::min<size_t>(left, 65533);
        metadata.insert(metadata.end(), {0xff, 0xe9, uint8_t((n + 2) >> 8), uint8_t((n + 2) & 0xff)});
        metadata.insert(metadata.end(), n, 0);
        left -= n;
    }
    out.insert(out.begin() + 2, metadata.begin(), metadata.end());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
    file.write(reinterpret_cast<const char *>(trailer.data()),
               static_cast<std::streamsize>(trailer.size()));
    return static_cast<bool>(file);
}

} // namespace testmedia
