#include "FlatVideoReader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace exportpipe {

namespace {

std::string avErr(const std::string &what, int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return what + ": " + buf;
}

// Clockwise rotation (0 / 90 / 180 / 270) a player applies to the stream.
int streamRotation(const AVStream *st)
{
    const AVPacketSideData *sd = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * 4)
        return 0;
    // av_display_rotation_get: counter-clockwise degrees.
    const double ccw = av_display_rotation_get(reinterpret_cast<const int32_t *>(sd->data));
    if (std::isnan(ccw))
        return 0;
    int cw = static_cast<int>(std::lround(-ccw)) % 360;
    if (cw < 0)
        cw += 360;
    return (cw + 45) / 90 % 4 * 90;
}

// swscale colour coefficients for a frame's colourspace.
int swsSpace(AVColorSpace cs, int height)
{
    switch (cs) {
    case AVCOL_SPC_BT709:
        return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return SWS_CS_ITU601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return SWS_CS_BT2020;
    default:
        return height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601; // untagged: guess by size
    }
}

// One swscale conversion into a caller's buffer, threaded: the "threads"
// option, which sws_scale_frame() splits the picture across (FFmpeg 5+;
// sws_scale() would run on one). Rebuilt when the conversion changes.
struct Scaler {
    struct Key {
        int srcW = 0, srcH = 0, srcFmt = -1, dstW = 0, dstH = 0, dstFmt = -1;
        int space = 0;   // SWS_CS_*: the source's matrix (RGB: none)
        int srcFull = 0; // the source's range
        bool operator==(const Key &) const = default;
    };
    SwsContext *sws = nullptr;
    Key key;
    AVFrame *src = av_frame_alloc();
    AVFrame *dst = av_frame_alloc();

    ~Scaler()
    {
        sws_freeContext(sws);
        av_frame_free(&src);
        av_frame_free(&dst);
    }

    bool setup(const Key &k)
    {
        if (sws && k == key)
            return true;
        sws_freeContext(sws);
        sws = sws_alloc_context();
        if (!sws)
            return false;
        av_opt_set_int(sws, "srcw", k.srcW, 0);
        av_opt_set_int(sws, "srch", k.srcH, 0);
        av_opt_set_int(sws, "src_format", k.srcFmt, 0);
        av_opt_set_int(sws, "dstw", k.dstW, 0);
        av_opt_set_int(sws, "dsth", k.dstH, 0);
        av_opt_set_int(sws, "dst_format", k.dstFmt, 0);
        av_opt_set_int(sws, "sws_flags", SWS_BILINEAR, 0);
        av_opt_set_int(sws, "threads", 0, 0); // auto
        if (sws_init_context(sws, nullptr, nullptr) < 0) {
            sws_freeContext(sws);
            sws = nullptr;
            return false;
        }
        // To RGBA: full range in the source's matrix. To NV12: BT.709
        // limited range, the export's.
        const bool toYuv = k.dstFmt == AV_PIX_FMT_NV12;
        sws_setColorspaceDetails(sws, sws_getCoefficients(k.space), k.srcFull,
                                 sws_getCoefficients(toYuv ? SWS_CS_ITU709 : k.space),
                                 toYuv ? 0 : 1, 0, 1 << 16, 1 << 16);
        key = k;
        return true;
    }

    static void noFree(void *, uint8_t *) {}

    // Point `f` at `data` (not owned): sws_scale_frame() wants reference
    // counted frames, and would allocate its own output otherwise.
    static void wrap(AVFrame *f, int w, int h, int fmt, uint8_t *const data[4],
                     const int linesize[4], size_t bytes)
    {
        av_frame_unref(f);
        f->width = w;
        f->height = h;
        f->format = fmt;
        for (int i = 0; i < 4; ++i) {
            f->data[i] = data[i];
            f->linesize[i] = linesize[i];
        }
        f->buf[0] = av_buffer_create(data[0], bytes, noFree, nullptr, 0);
    }

    // `in` (decoded, or wrapped) into `out` (dst planes of the key's size).
    bool scale(const AVFrame *in, uint8_t *const out[4], const int outStride[4], size_t outBytes)
    {
        wrap(dst, key.dstW, key.dstH, key.dstFmt, out, outStride, outBytes);
        const int r = dst->buf[0] ? sws_scale_frame(sws, dst, in) : -1;
        av_frame_unref(dst);
        return r >= 0;
    }
    bool scale(const uint8_t *in, int inStride, uint8_t *const out[4], const int outStride[4],
               size_t outBytes)
    {
        uint8_t *planes[4] = {const_cast<uint8_t *>(in), nullptr, nullptr, nullptr};
        const int strides[4] = {inStride, 0, 0, 0};
        wrap(src, key.srcW, key.srcH, key.srcFmt, planes, strides, size_t(inStride) * key.srcH);
        const bool ok = src->buf[0] && scale(src, out, outStride, outBytes);
        av_frame_unref(src);
        return ok;
    }
};

// NV12 planes in one buffer: luma, then the interleaved chroma.
void nv12Planes(uint8_t *data, int w, int h, uint8_t *planes[4], int strides[4])
{
    planes[0] = data;
    planes[1] = data + size_t(w) * h;
    planes[2] = planes[3] = nullptr;
    strides[0] = strides[1] = w;
    strides[2] = strides[3] = 0;
}

} // namespace

struct Nv12Converter::Impl {
    Scaler scaler;
};

Nv12Converter::Nv12Converter() : d(std::make_unique<Impl>()) {}
Nv12Converter::~Nv12Converter() = default;

bool Nv12Converter::convert(const uint8_t *rgba, int w, int h, std::vector<uint8_t> &nv12)
{
    Scaler::Key k;
    k.srcW = k.dstW = w;
    k.srcH = k.dstH = h;
    k.srcFmt = AV_PIX_FMT_RGBA;
    k.dstFmt = AV_PIX_FMT_NV12;
    k.space = SWS_CS_ITU709;
    k.srcFull = 1;
    if (w <= 0 || h <= 0 || (w | h) & 1 || !d->scaler.setup(k))
        return false;
    nv12.resize(size_t(w) * h * 3 / 2 + kPadding);
    uint8_t *planes[4];
    int strides[4];
    nv12Planes(nv12.data(), w, h, planes, strides);
    return d->scaler.scale(rgba, w * 4, planes, strides, nv12.size());
}

struct FlatVideoReader::Impl {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *ctx = nullptr;
    int streamIndex = -1;
    AVRational timeBase{1, 1000};
    int64_t startPts = 0;
    int rotation = 0; // clockwise degrees
    Info info;
    int outW = 0, outH = 0;
    bool eof = false;       // demuxer drained
    bool drained = false;   // decoder drained too
    AVPacket *pkt = nullptr;
    std::deque<AVFrame *> ahead; // decoded, not handed out yet
    std::string err;

    // frameAt(): the frame showing, raw, and its converted copy.
    AVFrame *cur = nullptr;
    VideoFrame curOut;
    bool curConverted = false;

    Format format = Format::Rgba;
    Scaler scaler;
    // swscale's RGBA output, before rotation; kPadding bytes longer
    // (convert()).
    std::vector<uint8_t> scaled;
    std::vector<uint8_t> rotated; // a rotated frame on its way to NV12
    Nv12Converter toNv12;
    static constexpr size_t kSwsSlack = Nv12Converter::kPadding;

    ~Impl() { close(); }

    void close()
    {
        for (AVFrame *f : ahead)
            av_frame_free(&f);
        ahead.clear();
        if (cur)
            av_frame_free(&cur);
        if (pkt)
            av_packet_free(&pkt);
        if (ctx)
            avcodec_free_context(&ctx);
        if (fmt)
            avformat_close_input(&fmt);
        streamIndex = -1;
    }

    int64_t ptsMs(const AVFrame *f) const
    {
        int64_t pts = f->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
            pts = f->pts;
        if (pts == AV_NOPTS_VALUE)
            return 0;
        return av_rescale_q(pts - startPts, timeBase, AVRational{1, 1000});
    }

    // The next decoded frame (owned by the caller), or null at the end.
    AVFrame *decodeOne()
    {
        AVFrame *f = av_frame_alloc();
        for (;;) {
            const int r = avcodec_receive_frame(ctx, f);
            if (r == 0)
                return f;
            if (r == AVERROR_EOF) {
                drained = true;
                break;
            }
            if (r != AVERROR(EAGAIN)) {
                err = avErr("decode", r);
                break;
            }
            if (eof) { // flushing, and nothing more came out
                drained = true;
                break;
            }
            const int rr = av_read_frame(fmt, pkt);
            if (rr < 0) {
                eof = true;
                avcodec_send_packet(ctx, nullptr); // flush
                continue;
            }
            if (pkt->stream_index == streamIndex)
                avcodec_send_packet(ctx, pkt);
            av_packet_unref(pkt);
        }
        av_frame_free(&f);
        return nullptr;
    }

    AVFrame *peek()
    {
        if (ahead.empty() && !drained)
            if (AVFrame *f = decodeOne())
                ahead.push_back(f);
        return ahead.empty() ? nullptr : ahead.front();
    }

    AVFrame *pop()
    {
        AVFrame *f = peek();
        if (f)
            ahead.pop_front();
        return f;
    }

    bool seek(int64_t ms)
    {
        for (AVFrame *f : ahead)
            av_frame_free(&f);
        ahead.clear();
        if (cur)
            av_frame_free(&cur);
        curConverted = false;
        const int64_t ts = startPts + av_rescale_q(std::max<int64_t>(0, ms), AVRational{1, 1000}, timeBase);
        const int r = av_seek_frame(fmt, streamIndex, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0 && ms > 0) {
            err = avErr("seek", r);
            return false;
        }
        avcodec_flush_buffers(ctx);
        eof = drained = false;
        // Decode up to the frame showing at `ms`; leave it at the front.
        AVFrame *prev = nullptr;
        for (;;) {
            AVFrame *f = peek();
            if (!f || (prev && ptsMs(f) > ms))
                break;
            ahead.pop_front();
            if (prev)
                av_frame_free(&prev);
            prev = f;
            if (ptsMs(f) >= ms)
                break;
        }
        if (prev)
            ahead.push_front(prev);
        return true;
    }

    // Convert `f` to the output format and size, rotation applied.
    void convert(const AVFrame *f, VideoFrame &out)
    {
        int w = outW > 0 ? outW : info.width;
        int h = outH > 0 ? outH : info.height;
        if (format == Format::Nv12) {
            w += w & 1;
            h += h & 1;
        }
        out.ptsMs = ptsMs(f);
        out.width = w;
        out.height = h;
        const int space = swsSpace(f->colorspace, f->height);
        const int srcFull = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        if (format == Format::Nv12) {
            // Straight to NV12 when there's nothing to turn and the matrix is
            // already the export's (swscale doesn't convert between YUV
            // matrices); else via RGBA.
            if (!rotation && space == SWS_CS_ITU709 &&
                scaleTo(f, w, h, AV_PIX_FMT_NV12, space, srcFull, out.nv12))
                return;
            rotated.resize(size_t(w) * h * 4 + kSwsSlack);
            if (!toRgba(f, w, h, space, srcFull, rotated.data()) ||
                !toNv12.convert(rotated.data(), w, h, out.nv12))
                blackNv12(w, h, out.nv12);
            return;
        }
        out.rgba.resize(size_t(w) * h * 4);
        if (!toRgba(f, w, h, space, srcFull, out.rgba.data()))
            std::fill(out.rgba.begin(), out.rgba.end(), 0);
    }

    // `f` scaled to w x h in `fmt` into `buf` (resized, kSwsSlack longer).
    bool scaleTo(const AVFrame *f, int w, int h, AVPixelFormat fmt, int space, int srcFull,
                 std::vector<uint8_t> &buf)
    {
        Scaler::Key k{f->width, f->height, f->format, w, h, fmt, space, srcFull};
        if (!scaler.setup(k))
            return false;
        uint8_t *planes[4];
        int strides[4];
        if (fmt == AV_PIX_FMT_NV12) {
            buf.resize(size_t(w) * h * 3 / 2 + kSwsSlack);
            nv12Planes(buf.data(), w, h, planes, strides);
        } else {
            buf.resize(size_t(w) * h * 4 + kSwsSlack);
            planes[0] = buf.data();
            planes[1] = planes[2] = planes[3] = nullptr;
            strides[0] = w * 4;
            strides[1] = strides[2] = strides[3] = 0;
        }
        return scaler.scale(f, planes, strides, buf.size());
    }

    static void blackNv12(int w, int h, std::vector<uint8_t> &buf)
    {
        buf.assign(size_t(w) * h * 3 / 2 + kSwsSlack, 128);
        std::fill_n(buf.begin(), size_t(w) * h, 16);
    }

    // `f` as RGBA at w x h, rotation applied, into `dst` (w * h * 4).
    bool toRgba(const AVFrame *f, int w, int h, int space, int srcFull, uint8_t *dst)
    {
        // Scale into the pre-rotation shape, then rotate. Into a scratch
        // buffer with room to spare: swscale's unscaled converters (output
        // size = the frame's) write whole SIMD blocks, up to tens of bytes
        // past the last pixel when a row isn't a multiple of the block -- a
        // 1080-wide frame does that.
        const bool swap = rotation == 90 || rotation == 270;
        const int sw = swap ? h : w, sh = swap ? w : h;
        if (!scaleTo(f, sw, sh, AV_PIX_FMT_RGBA, space, srcFull, scaled))
            return false;
        if (!rotation) {
            std::memcpy(dst, scaled.data(), size_t(w) * h * 4);
            return true;
        }
        // Rotate the sw x sh buffer clockwise into the w x h output.
        const uint32_t *src = reinterpret_cast<const uint32_t *>(scaled.data());
        uint32_t *o = reinterpret_cast<uint32_t *>(dst);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int sx, sy;
                if (rotation == 90) {
                    sx = y;
                    sy = sh - 1 - x;
                } else if (rotation == 180) {
                    sx = sw - 1 - x;
                    sy = sh - 1 - y;
                } else { // 270
                    sx = sw - 1 - y;
                    sy = x;
                }
                o[size_t(y) * w + x] = src[size_t(sy) * sw + sx];
            }
        return true;
    }
};

FlatVideoReader::FlatVideoReader() : d(std::make_unique<Impl>()) {}
FlatVideoReader::~FlatVideoReader() = default;

bool FlatVideoReader::probe(const std::string &path, Info &info, std::string &err)
{
    FlatVideoReader r;
    if (!r.open(path, err))
        return false;
    info = r.info();
    return true;
}

bool FlatVideoReader::open(const std::string &path, std::string &err)
{
    d->close();
    d->err.clear();
    int r = avformat_open_input(&d->fmt, path.c_str(), nullptr, nullptr);
    if (r < 0) {
        err = avErr("open " + path, r);
        return false;
    }
    if ((r = avformat_find_stream_info(d->fmt, nullptr)) < 0) {
        err = avErr("read stream info", r);
        d->close();
        return false;
    }
    const AVCodec *dec = nullptr;
    d->streamIndex = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (d->streamIndex < 0 || !dec) {
        err = "no video stream in " + path;
        d->close();
        return false;
    }
    AVStream *st = d->fmt->streams[d->streamIndex];
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        err = "no video stream in " + path + " (only cover art)";
        d->close();
        return false;
    }
    d->ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(d->ctx, st->codecpar);
    d->ctx->thread_count = 0; // auto
    d->ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if ((r = avcodec_open2(d->ctx, dec, nullptr)) < 0) {
        err = avErr("open decoder", r);
        d->close();
        return false;
    }
    d->pkt = av_packet_alloc();
    d->timeBase = st->time_base;
    d->startPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    d->rotation = streamRotation(st);

    int w = st->codecpar->width, h = st->codecpar->height;
    const AVRational sar = av_guess_sample_aspect_ratio(d->fmt, st, nullptr);
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den)
        w = static_cast<int>(std::lround(double(w) * sar.num / sar.den));
    if (d->rotation == 90 || d->rotation == 270)
        std::swap(w, h);
    d->info.width = w;
    d->info.height = h;
    if (st->duration != AV_NOPTS_VALUE)
        d->info.durationMs = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000});
    else if (d->fmt->duration != AV_NOPTS_VALUE)
        d->info.durationMs = d->fmt->duration / 1000;
    const AVRational fr = av_guess_frame_rate(d->fmt, st, nullptr);
    d->info.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0.0;
    if (w <= 0 || h <= 0 || d->info.durationMs <= 0 || d->info.fps <= 0.0) {
        err = "cannot tell the size, length or frame rate of the video in " + path;
        d->close();
        return false;
    }
    d->eof = d->drained = false;
    return true;
}

bool FlatVideoReader::isOpen() const { return d->streamIndex >= 0; }
const FlatVideoReader::Info &FlatVideoReader::info() const { return d->info; }
const std::string &FlatVideoReader::lastError() const { return d->err; }

void FlatVideoReader::setOutputSize(int width, int height)
{
    width = std::max(2, width);
    height = std::max(2, height);
    if (width == d->outW && height == d->outH)
        return;
    d->outW = width;
    d->outH = height;
    d->curConverted = false;
}

void FlatVideoReader::setOutputFormat(Format format)
{
    if (format == d->format)
        return;
    d->format = format;
    d->curConverted = false;
}

bool FlatVideoReader::seek(int64_t ms)
{
    return isOpen() && d->seek(ms);
}

bool FlatVideoReader::readNext(VideoFrame &out)
{
    if (!isOpen())
        return false;
    AVFrame *f = d->pop();
    if (!f)
        return false;
    d->convert(f, out);
    // It is now the frame showing, for a later frameAt().
    if (d->cur)
        av_frame_free(&d->cur);
    d->cur = f;
    d->curConverted = false;
    return true;
}

const VideoFrame *FlatVideoReader::frameAt(int64_t ms)
{
    if (!isOpen())
        return nullptr;
    constexpr int64_t kSeekAheadMs = 2000; // further than this: seek, don't decode through
    const bool behind = d->cur && ms < d->ptsMs(d->cur);
    const AVFrame *next = d->cur ? d->peek() : nullptr;
    const bool farAhead = d->cur && next && ms > d->ptsMs(next) + kSeekAheadMs;
    if (!d->cur || behind || farAhead) {
        if (!d->seek(ms))
            return nullptr;
        d->cur = d->pop();
        d->curConverted = false;
        if (!d->cur)
            return nullptr;
    }
    // Step forward while the next frame is already due.
    while (AVFrame *n = d->peek()) {
        if (d->ptsMs(n) > ms)
            break;
        d->ahead.pop_front();
        av_frame_free(&d->cur);
        d->cur = n;
        d->curConverted = false;
    }
    if (!d->curConverted) {
        d->convert(d->cur, d->curOut);
        d->curConverted = true;
    }
    return &d->curOut;
}

} // namespace exportpipe
