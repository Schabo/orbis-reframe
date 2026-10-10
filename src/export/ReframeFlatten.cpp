#include "ReframeFlatten.h"

#include "ReframeRenderer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace exportpipe {

namespace {

std::string avErr(const std::string& what, int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return what + ": " + buf;
}

struct Encoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVStream* stream = nullptr;
    SwsContext* toNv12 = nullptr;
    AVFrame* nv12 = nullptr;
    AVPacket* pkt = nullptr;
    // Optional AAC audio stream (FlattenConfig::audio_sample_rate > 0).
    AVCodecContext* actx = nullptr;
    AVStream* astream = nullptr;
    AVFrame* aframe = nullptr;
};

void closeEncoder(Encoder& e)
{
    if (e.aframe)
        av_frame_free(&e.aframe);
    if (e.actx)
        avcodec_free_context(&e.actx);
    if (e.toNv12) {
        sws_freeContext(e.toNv12);
        e.toNv12 = nullptr;
    }
    if (e.nv12)
        av_frame_free(&e.nv12);
    if (e.pkt)
        av_packet_free(&e.pkt);
    if (e.ctx)
        avcodec_free_context(&e.ctx);
    if (e.fmt) {
        if (!(e.fmt->oformat->flags & AVFMT_NOFILE) && e.fmt->pb)
            avio_closep(&e.fmt->pb);
        avformat_free_context(e.fmt);
        e.fmt = nullptr;
    }
}

bool openEncoder(const FlattenConfig& p, Encoder& e, std::string& err)
{
    const char* encName = p.h265 ? "hevc_nvenc" : "h264_nvenc";
    const AVCodec* enc = avcodec_find_encoder_by_name(encName);
    if (!enc) {
        err = std::string(encName) + " not available (NVIDIA driver loaded? NVENC-enabled ffmpeg?)";
        return false;
    }

    avformat_alloc_output_context2(&e.fmt, nullptr, nullptr, p.out_path.c_str());
    if (!e.fmt) {
        err = "cannot infer output container from " + p.out_path;
        return false;
    }

    e.ctx = avcodec_alloc_context3(enc);
    e.ctx->width = p.out_w;
    e.ctx->height = p.out_h;
    const AVRational tb = av_d2q(1.0 / p.fps, 1000000);
    e.ctx->time_base = tb;
    e.ctx->framerate = av_inv_q(tb);
    e.ctx->pix_fmt = AV_PIX_FMT_NV12;
    // BT.709 limited range, matching reframe_reproject.comp and the swscale
    // setup below; tagged so players don't have to guess.
    e.ctx->color_range = AVCOL_RANGE_MPEG;
    e.ctx->colorspace = AVCOL_SPC_BT709;
    e.ctx->color_primaries = AVCOL_PRI_BT709;
    e.ctx->color_trc = AVCOL_TRC_BT709;
    e.ctx->bit_rate = p.bitrate_kbps * 1000;
    // VBR peak headroom for busy scenes; the average stays at bit_rate.
    e.ctx->rc_max_rate = e.ctx->bit_rate * 3 / 2;
    e.ctx->gop_size = std::max(1, static_cast<int>(std::lround(p.fps)) * 2);
    if (e.fmt->oformat->flags & AVFMT_GLOBALHEADER)
        e.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(e.ctx->priv_data, "preset", "p5", 0);
    av_opt_set(e.ctx->priv_data, "tune", "hq", 0);
    av_opt_set(e.ctx->priv_data, "rc", "vbr", 0);

    int ret = avcodec_open2(e.ctx, enc, nullptr);
    if (ret < 0) {
        err = avErr(std::string("open ") + encName, ret);
        return false;
    }

    e.stream = avformat_new_stream(e.fmt, nullptr);
    avcodec_parameters_from_context(e.stream->codecpar, e.ctx);
    e.stream->time_base = e.ctx->time_base;

    if (p.audio_sample_rate > 0) {
        const AVCodec* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aenc) {
            err = "no AAC encoder in this FFmpeg build";
            return false;
        }
        e.actx = avcodec_alloc_context3(aenc);
        e.actx->sample_fmt = AV_SAMPLE_FMT_FLTP; // FFmpeg's native aac encoder's only format
        e.actx->sample_rate = p.audio_sample_rate;
        av_channel_layout_default(&e.actx->ch_layout, p.audio_channels);
        e.actx->bit_rate = p.audio_bitrate_kbps * 1000;
        e.actx->time_base = AVRational{1, p.audio_sample_rate};
        if (e.fmt->oformat->flags & AVFMT_GLOBALHEADER)
            e.actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((ret = avcodec_open2(e.actx, aenc, nullptr)) < 0) {
            err = avErr("open AAC encoder", ret);
            return false;
        }
        e.astream = avformat_new_stream(e.fmt, nullptr);
        avcodec_parameters_from_context(e.astream->codecpar, e.actx);
        e.astream->time_base = e.actx->time_base;

        e.aframe = av_frame_alloc();
        e.aframe->format = e.actx->sample_fmt;
        e.aframe->sample_rate = e.actx->sample_rate;
        av_channel_layout_copy(&e.aframe->ch_layout, &e.actx->ch_layout);
        e.aframe->nb_samples = e.actx->frame_size > 0 ? e.actx->frame_size : 1024;
        if ((ret = av_frame_get_buffer(e.aframe, 0)) < 0) {
            err = avErr("allocate audio frame", ret);
            return false;
        }
    }

    if (!(e.fmt->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&e.fmt->pb, p.out_path.c_str(), AVIO_FLAG_WRITE)) < 0) {
            err = avErr("open output " + p.out_path, ret);
            return false;
        }
    }
    if ((ret = avformat_write_header(e.fmt, nullptr)) < 0) {
        err = avErr("write output header", ret);
        return false;
    }

    e.nv12 = av_frame_alloc();
    e.nv12->format = AV_PIX_FMT_NV12;
    e.nv12->width = p.out_w;
    e.nv12->height = p.out_h;
    av_frame_get_buffer(e.nv12, 0);
    e.pkt = av_packet_alloc();
    e.toNv12 = sws_getContext(p.out_w, p.out_h, AV_PIX_FMT_RGBA, p.out_w, p.out_h, AV_PIX_FMT_NV12,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!e.toNv12) {
        err = "cannot create RGBA->NV12 scaler";
        return false;
    }
    // swscale's RGB->YUV default is BT.601; make it BT.709 limited range.
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(e.toNv12, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
    return true;
}

// Send `frame` (nullptr => flush) to `ctx` and mux every packet it yields
// into `stream`. Shared by the video and the audio stream.
bool drainTo(Encoder& e, AVCodecContext* ctx, AVStream* stream, AVFrame* frame, std::string& err)
{
    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        err = avErr("avcodec_send_frame", ret);
        return false;
    }
    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, e.pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            err = avErr("avcodec_receive_packet", ret);
            return false;
        }
        av_packet_rescale_ts(e.pkt, ctx->time_base, stream->time_base);
        e.pkt->stream_index = stream->index;
        av_interleaved_write_frame(e.fmt, e.pkt);
        av_packet_unref(e.pkt);
    }
    return true;
}

bool drainEncoder(Encoder& e, AVFrame* frame, std::string& err)
{
    return drainTo(e, e.ctx, e.stream, frame, err);
}

} // namespace

// --- FlattenWriter ---------------------------------------------------------

struct FlattenWriter::Impl {
    FlattenConfig cfg;
    Encoder enc;
    bool opened = false;
    int64_t written = 0; // shared across every input -> continuous output PTS
    std::vector<uint8_t> flatRgba; // reused across inputs (output-sized)
    // Audio: interleaved PCM not yet a whole AAC frame, and the running
    // sample count (fed, and encoded -> the next audio frame's PTS).
    std::vector<float> audioPending;
    int64_t audioFed = 0;
    int64_t audioEncoded = 0;
    FrameHook hook;

    // Run the hook on the NV12 frame about to be encoded.
    bool runHook(std::string& err)
    {
        if (!hook)
            return true;
        return hook(written, enc.nv12->data[0], enc.nv12->linesize[0], enc.nv12->data[1],
                    enc.nv12->linesize[1], err);
    }

    // Encode `frames` frames from the front of audioPending (<= one AAC frame).
    bool encodeAudioFrame(int frames, std::string& err);
};

bool FlattenWriter::Impl::encodeAudioFrame(int frames, std::string& err)
{
    AVFrame* f = enc.aframe;
    av_frame_make_writable(f);
    const int ch = cfg.audio_channels;
    f->nb_samples = frames;
    for (int c = 0; c < ch; ++c) {
        float* plane = reinterpret_cast<float*>(f->data[c]);
        for (int i = 0; i < frames; ++i)
            plane[i] = audioPending[static_cast<size_t>(i) * ch + c];
    }
    f->pts = audioEncoded;
    audioEncoded += frames;
    audioPending.erase(audioPending.begin(), audioPending.begin() + static_cast<ptrdiff_t>(frames) * ch);
    return drainTo(enc, enc.actx, enc.astream, f, err);
}

FlattenWriter::FlattenWriter() : d(std::make_unique<Impl>()) {}

FlattenWriter::~FlattenWriter()
{
    if (d)
        closeEncoder(d->enc);
}

bool FlattenWriter::open(const FlattenConfig& cfg, std::string& err)
{
    d->cfg = cfg;
    if (!openEncoder(d->cfg, d->enc, err)) {
        closeEncoder(d->enc);
        return false;
    }
    d->flatRgba.assign(static_cast<size_t>(cfg.out_w) * cfg.out_h * 4, 0);
    d->opened = true;
    return true;
}

bool FlattenWriter::encodeFrame(const uint8_t* eqRgba, int eqW, int eqH, const ReframeView& view,
                                std::string& err)
{
    RenderRectilinear(eqRgba, eqW, eqH, d->flatRgba.data(), d->cfg.out_w, d->cfg.out_h, view);

    av_frame_make_writable(d->enc.nv12);
    const uint8_t* src[1] = {d->flatRgba.data()};
    int srcStride[1] = {d->cfg.out_w * 4};
    sws_scale(d->enc.toNv12, src, srcStride, 0, d->cfg.out_h, d->enc.nv12->data,
              d->enc.nv12->linesize);
    if (!d->runHook(err))
        return false;
    d->enc.nv12->pts = d->written;
    if (!drainEncoder(d->enc, d->enc.nv12, err))
        return false;
    ++d->written;
    return true;
}

bool FlattenWriter::addEquirectFrame(const uint8_t* eqRgba, int eqW, int eqH, const float pose[4],
                                     const float stab[9], const float colorGrade[4],
                                     std::string& err)
{
    if (!d->opened) {
        err = "FlattenWriter::addEquirectFrame before open()";
        return false;
    }
    ReframeView view{};
    view.aspect = static_cast<float>(d->cfg.out_w) / static_cast<float>(d->cfg.out_h);
    view.fovY = 1.309f;
    if (pose) {
        view.yaw = pose[0];
        view.pitch = pose[1];
        view.roll = pose[2];
        view.fovY = pose[3];
    }
    if (stab) {
        for (int i = 0; i < 9; ++i)
            view.stab[i] = stab[i];
    }
    if (colorGrade) {
        for (int i = 0; i < 4; ++i)
            view.colorGrade[i] = colorGrade[i];
    }
    return encodeFrame(eqRgba, eqW, eqH, view, err);
}

bool FlattenWriter::addNv12Frame(const uint8_t* nv12, int w, int h, std::string& err)
{
    if (!d->opened) {
        err = "FlattenWriter::addNv12Frame before open()";
        return false;
    }
    if (w != d->cfg.out_w || h != d->cfg.out_h) {
        err = "addNv12Frame size does not match the FlattenConfig output size";
        return false;
    }

    av_frame_make_writable(d->enc.nv12);
    const uint8_t* srcY = nv12;
    const uint8_t* srcUV = nv12 + static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y)
        std::memcpy(d->enc.nv12->data[0] + static_cast<ptrdiff_t>(y) * d->enc.nv12->linesize[0],
                    srcY + static_cast<size_t>(y) * w, w);
    for (int y = 0; y < h / 2; ++y)
        std::memcpy(d->enc.nv12->data[1] + static_cast<ptrdiff_t>(y) * d->enc.nv12->linesize[1],
                    srcUV + static_cast<size_t>(y) * w, w);
    if (!d->runHook(err))
        return false;
    d->enc.nv12->pts = d->written;
    if (!drainEncoder(d->enc, d->enc.nv12, err))
        return false;
    ++d->written;
    return true;
}

bool FlattenWriter::addAudio(const float* interleaved, int64_t frames, std::string& err)
{
    if (!d->opened || !d->enc.actx) {
        err = "FlattenWriter::addAudio without an open audio stream";
        return false;
    }
    if (frames <= 0)
        return true;
    const int ch = d->cfg.audio_channels;
    d->audioPending.insert(d->audioPending.end(), interleaved, interleaved + frames * ch);
    d->audioFed += frames;
    const int frameSize = d->enc.actx->frame_size > 0 ? d->enc.actx->frame_size : 1024;
    while (d->audioPending.size() >= static_cast<size_t>(frameSize) * ch) {
        if (!d->encodeAudioFrame(frameSize, err))
            return false;
    }
    return true;
}

int64_t FlattenWriter::audioFramesWritten() const { return d->audioFed; }

void FlattenWriter::setFrameHook(FrameHook hook) { d->hook = std::move(hook); }

int64_t FlattenWriter::finish(std::string& err)
{
    if (!d->opened) {
        err = "FlattenWriter::finish before open()";
        return -1;
    }
    if (!drainEncoder(d->enc, nullptr, err)) // flush encoder
        return -1;
    if (d->enc.actx) {
        // The tail (< one AAC frame): pad it to a whole frame unless the
        // encoder takes a short last frame (FFmpeg's aac does).
        const int ch = d->cfg.audio_channels;
        int tail = static_cast<int>(d->audioPending.size() / ch);
        if (tail > 0) {
            const int frameSize = d->enc.actx->frame_size > 0 ? d->enc.actx->frame_size : 1024;
            if (!(d->enc.actx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) {
                d->audioPending.resize(static_cast<size_t>(frameSize) * ch, 0.0f);
                tail = frameSize;
            }
            if (!d->encodeAudioFrame(tail, err))
                return -1;
        }
        if (!drainTo(d->enc, d->enc.actx, d->enc.astream, nullptr, err))
            return -1;
    }
    int ret = av_write_trailer(d->enc.fmt);
    if (ret < 0) {
        err = avErr("write output trailer", ret);
        return -1;
    }
    d->opened = false;
    return d->written;
}

} // namespace exportpipe
