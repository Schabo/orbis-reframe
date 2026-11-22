#include "support/FfmpegFaults.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
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

constexpr int kCalls = 5;
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
int __real_avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt);
int __real_avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame);

int __wrap_av_read_frame(AVFormatContext *s, AVPacket *pkt)
{
    if (ffmpegfaults::fails(Call::ReadFrame))
        return ffmpegfaults::rcOf(Call::ReadFrame);
    return __real_av_read_frame(s, pkt);
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

} // extern "C"
