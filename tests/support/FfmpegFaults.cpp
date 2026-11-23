#include "support/FfmpegFaults.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <set>

namespace ffmpegfaults {

namespace {

struct Fault {
    int after = 0;
    int rc = 0;
    int times = 0; // 0: not armed
    int calls = 0;
    int failed = 0;
};

constexpr int kCalls = 18;
Fault g_faults[kCalls];
// The decoders that took the flush since reset(), for Call::ReceiveDraining.
std::set<const AVCodecContext *> g_draining;

// True if this call of `call` fails.
bool fails(Call call)
{
    Fault &f = g_faults[static_cast<int>(call)];
    if (f.times == 0)
        return false;
    if (f.calls++ < f.after)
        return false;
    if (f.times > 0 && f.failed >= f.times)
        return false;
    ++f.failed;
    return true;
}

int rcOf(Call call) { return g_faults[static_cast<int>(call)].rc; }

} // namespace

void arm(Call call, int after, int rc, int times)
{
    g_faults[static_cast<int>(call)] = Fault{after, rc, times, 0, 0};
}

void reset()
{
    for (Fault &f : g_faults)
        f = Fault{};
    g_draining.clear();
}

int failed(Call call) { return g_faults[static_cast<int>(call)].failed; }

} // namespace ffmpegfaults

using ffmpegfaults::Call;

extern "C" {

int __real_av_read_frame(AVFormatContext *s, AVPacket *pkt);
int __real_av_seek_frame(AVFormatContext *s, int stream_index, int64_t timestamp, int flags);
int __real_avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt);
int __real_avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame);
AVCodecContext *__real_avcodec_alloc_context3(const AVCodec *codec);
int __real_avcodec_parameters_to_context(AVCodecContext *codec, const AVCodecParameters *par);
AVPacket *__real_av_packet_alloc(void);
AVFrame *__real_av_frame_alloc(void);
AVBufferRef *__real_av_buffer_ref(const AVBufferRef *buf);
SwsContext *__real_sws_getContext(int srcW, int srcH, AVPixelFormat srcFormat, int dstW, int dstH,
                                  AVPixelFormat dstFormat, int flags, SwsFilter *srcFilter,
                                  SwsFilter *dstFilter, const double *param);
SwsContext *__real_sws_getCachedContext(SwsContext *context, int srcW, int srcH,
                                        AVPixelFormat srcFormat, int dstW, int dstH,
                                        AVPixelFormat dstFormat, int flags, SwsFilter *srcFilter,
                                        SwsFilter *dstFilter, const double *param);
int __real_sws_init_context(SwsContext *sws_context, SwsFilter *srcFilter, SwsFilter *dstFilter);
int __real_sws_scale(SwsContext *c, const uint8_t *const srcSlice[], const int srcStride[],
                     int srcSliceY, int srcSliceH, uint8_t *const dst[], const int dstStride[]);
int __real_sws_scale_frame(SwsContext *c, AVFrame *dst, const AVFrame *src);
AVStream *__real_avformat_new_stream(AVFormatContext *s, const AVCodec *c);
int __real_av_frame_get_buffer(AVFrame *frame, int align);
int __real_av_frame_make_writable(AVFrame *frame);
int __real_av_interleaved_write_frame(AVFormatContext *s, AVPacket *pkt);
int __real_avio_closep(AVIOContext **s);

int __wrap_av_read_frame(AVFormatContext *s, AVPacket *pkt)
{
    if (ffmpegfaults::fails(Call::ReadFrame))
        return ffmpegfaults::rcOf(Call::ReadFrame);
    return __real_av_read_frame(s, pkt);
}

int __wrap_av_seek_frame(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    if (ffmpegfaults::fails(Call::SeekFrame))
        return ffmpegfaults::rcOf(Call::SeekFrame);
    return __real_av_seek_frame(s, stream_index, timestamp, flags);
}

int __wrap_avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    const Call call = avpkt ? Call::SendPacket : Call::SendFlush;
    if (ffmpegfaults::fails(call))
        return ffmpegfaults::rcOf(call);
    const int r = __real_avcodec_send_packet(avctx, avpkt);
    if (!avpkt && r == 0)
        ffmpegfaults::g_draining.insert(avctx);
    return r;
}

int __wrap_avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    if (ffmpegfaults::fails(Call::ReceiveFrame))
        return ffmpegfaults::rcOf(Call::ReceiveFrame);
    if (ffmpegfaults::g_draining.count(avctx) && ffmpegfaults::fails(Call::ReceiveDraining))
        return ffmpegfaults::rcOf(Call::ReceiveDraining);
    return __real_avcodec_receive_frame(avctx, frame);
}

AVCodecContext *__wrap_avcodec_alloc_context3(const AVCodec *codec)
{
    return ffmpegfaults::fails(Call::AllocContext) ? nullptr : __real_avcodec_alloc_context3(codec);
}

int __wrap_avcodec_parameters_to_context(AVCodecContext *codec, const AVCodecParameters *par)
{
    if (ffmpegfaults::fails(Call::ParamsToContext))
        return ffmpegfaults::rcOf(Call::ParamsToContext);
    return __real_avcodec_parameters_to_context(codec, par);
}

AVPacket *__wrap_av_packet_alloc(void)
{
    return ffmpegfaults::fails(Call::PacketAlloc) ? nullptr : __real_av_packet_alloc();
}

AVFrame *__wrap_av_frame_alloc(void)
{
    return ffmpegfaults::fails(Call::FrameAlloc) ? nullptr : __real_av_frame_alloc();
}

AVBufferRef *__wrap_av_buffer_ref(const AVBufferRef *buf)
{
    return ffmpegfaults::fails(Call::BufferRef) ? nullptr : __real_av_buffer_ref(buf);
}

SwsContext *__wrap_sws_getContext(int srcW, int srcH, AVPixelFormat srcFormat, int dstW, int dstH,
                                  AVPixelFormat dstFormat, int flags, SwsFilter *srcFilter,
                                  SwsFilter *dstFilter, const double *param)
{
    if (ffmpegfaults::fails(Call::ScalerSetup))
        return nullptr;
    return __real_sws_getContext(srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags, srcFilter,
                                 dstFilter, param);
}

SwsContext *__wrap_sws_getCachedContext(SwsContext *context, int srcW, int srcH,
                                        AVPixelFormat srcFormat, int dstW, int dstH,
                                        AVPixelFormat dstFormat, int flags, SwsFilter *srcFilter,
                                        SwsFilter *dstFilter, const double *param)
{
    if (ffmpegfaults::fails(Call::ScalerSetup)) {
        sws_freeContext(context); // as FFmpeg does when it can't make the new one
        return nullptr;
    }
    return __real_sws_getCachedContext(context, srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags,
                                       srcFilter, dstFilter, param);
}

int __wrap_sws_init_context(SwsContext *sws_context, SwsFilter *srcFilter, SwsFilter *dstFilter)
{
    if (ffmpegfaults::fails(Call::ScalerSetup))
        return ffmpegfaults::rcOf(Call::ScalerSetup);
    return __real_sws_init_context(sws_context, srcFilter, dstFilter);
}

int __wrap_sws_scale(SwsContext *c, const uint8_t *const srcSlice[], const int srcStride[],
                     int srcSliceY, int srcSliceH, uint8_t *const dst[], const int dstStride[])
{
    if (ffmpegfaults::fails(Call::Scale))
        return ffmpegfaults::rcOf(Call::Scale);
    return __real_sws_scale(c, srcSlice, srcStride, srcSliceY, srcSliceH, dst, dstStride);
}

int __wrap_sws_scale_frame(SwsContext *c, AVFrame *dst, const AVFrame *src)
{
    if (ffmpegfaults::fails(Call::Scale))
        return ffmpegfaults::rcOf(Call::Scale);
    return __real_sws_scale_frame(c, dst, src);
}

AVStream *__wrap_avformat_new_stream(AVFormatContext *s, const AVCodec *c)
{
    return ffmpegfaults::fails(Call::NewStream) ? nullptr : __real_avformat_new_stream(s, c);
}

int __wrap_av_frame_get_buffer(AVFrame *frame, int align)
{
    if (ffmpegfaults::fails(Call::FrameBuffer))
        return ffmpegfaults::rcOf(Call::FrameBuffer);
    return __real_av_frame_get_buffer(frame, align);
}

int __wrap_av_frame_make_writable(AVFrame *frame)
{
    if (ffmpegfaults::fails(Call::MakeWritable))
        return ffmpegfaults::rcOf(Call::MakeWritable);
    return __real_av_frame_make_writable(frame);
}

int __wrap_av_interleaved_write_frame(AVFormatContext *s, AVPacket *pkt)
{
    if (ffmpegfaults::fails(Call::WritePacket)) {
        if (pkt)
            av_packet_unref(pkt); // the muxer takes the packet either way
        return ffmpegfaults::rcOf(Call::WritePacket);
    }
    return __real_av_interleaved_write_frame(s, pkt);
}

int __wrap_avio_closep(AVIOContext **s)
{
    const int r = __real_avio_closep(s);
    if (ffmpegfaults::fails(Call::CloseOutput))
        return ffmpegfaults::rcOf(Call::CloseOutput);
    return r;
}

} // extern "C"
