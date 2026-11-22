// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/media/FisheyeDemux.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace orbiscore {

FisheyeDemux::~FisheyeDemux() { close(); }

void FisheyeDemux::close()
{
    m_demux.close();
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_swsSrcFullRange = -1;
}

namespace {
bool CpuDecoderSetup(const AVStream*, const AVCodec*, AVCodecContext* ctx, std::string&)
{
    ctx->thread_count = 0; // let FFmpeg pick; frame+slice threading
    return true;
}
} // namespace

bool FisheyeDemux::open(const std::string& path, std::string& err)
{
    close();
    return m_demux.open(path, CpuDecoderSetup, err);
}

bool FisheyeDemux::open(const std::vector<LensSource>& lenses, std::string& err)
{
    return open(std::vector<ClipPart>{{0, lenses}}, err);
}

bool FisheyeDemux::open(const std::vector<ClipPart>& parts, std::string& err)
{
    close();
    for (size_t i = 0; i < 2; ++i)
        m_rect[i] =
            !parts.empty() && i < parts[0].lenses.size() ? parts[0].lenses[i].rect : LensRect{};
    return m_demux.open(parts, CpuDecoderSetup, err);
}

void FisheyeDemux::crop(int lens, Image& img) const
{
    const LensRect& r = m_rect[lens];
    if (r.x == 0.0 && r.y == 0.0 && r.w == 1.0 && r.h == 1.0)
        return;
    const int x = static_cast<int>(std::lround(r.x * img.w));
    const int y = static_cast<int>(std::lround(r.y * img.h));
    const int w = std::min(static_cast<int>(std::lround(r.w * img.w)), img.w - x);
    const int h = std::min(static_cast<int>(std::lround(r.h * img.h)), img.h - y);
    Image out;
    out.alloc(std::max(w, 0), std::max(h, 0));
    for (int row = 0; row < out.h; ++row)
        std::memcpy(out.rgb.data() + static_cast<size_t>(row) * out.w * 3,
                    img.rgb.data() + (static_cast<size_t>(y + row) * img.w + x) * 3,
                    static_cast<size_t>(out.w) * 3);
    img = std::move(out);
}

bool FisheyeDemux::convert(const AVFrame* frm, const InsvStreamInfo& stream, Image& out,
                           std::string& err)
{
    out.alloc(frm->width, frm->height);

    // The camera's HEVC decodes as a deprecated full-range JPEG pixel format
    // (yuvj420p); swscale wants the plain format, with the range set apart.
    // The matrix and range are the stream's (LensDemuxer), the same the GPU
    // stitch's ycbcr conversion takes, so both paths give the same RGB.
    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frm->format);
    switch (srcFmt) {
    case AV_PIX_FMT_YUVJ420P: srcFmt = AV_PIX_FMT_YUV420P; break;
    case AV_PIX_FMT_YUVJ422P: srcFmt = AV_PIX_FMT_YUV422P; break;
    case AV_PIX_FMT_YUVJ444P: srcFmt = AV_PIX_FMT_YUV444P; break;
    case AV_PIX_FMT_YUVJ440P: srcFmt = AV_PIX_FMT_YUV440P; break;
    case AV_PIX_FMT_YUVJ411P: srcFmt = AV_PIX_FMT_YUV411P; break;
    default: break;
    }
    const int srcFullRange = stream.ycbcr.fullRange ? 1 : 0;
    const int coefId = stream.ycbcr.matrix == YcbcrMatrix::Bt709   ? SWS_CS_ITU709
                       : stream.ycbcr.matrix == YcbcrMatrix::Bt2020 ? SWS_CS_BT2020
                                                              : SWS_CS_ITU601;

    SwsContext* prev = m_sws;
    // On failure the old context is freed: the next call starts afresh.
    m_sws = sws_getCachedContext(m_sws, frm->width, frm->height, srcFmt, frm->width, frm->height,
                                 AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) {
        err = "can't set up the RGB conversion of the " + std::to_string(frm->width) + "x" +
              std::to_string(frm->height) + " frame";
        out = {};
        return false;
    }
    if (m_sws != prev || srcFullRange != m_swsSrcFullRange || coefId != m_swsCoefId) {
        const int* coef = sws_getCoefficients(coefId);
        // src range = srcFullRange, dst (RGB) range = full; neutral bright/contrast/sat.
        sws_setColorspaceDetails(m_sws, coef, srcFullRange, coef, 1, 0, 1 << 16, 1 << 16);
        m_swsSrcFullRange = srcFullRange;
        m_swsCoefId = coefId;
    }

    uint8_t* dst[1] = {out.rgb.data()};
    int stride[1] = {out.w * 3};
    if (const int r = sws_scale(m_sws, frm->data, frm->linesize, 0, frm->height, dst, stride);
        r <= 0) {
        char why[AV_ERROR_MAX_STRING_SIZE] = {0};
        if (r < 0)
            av_strerror(r, why, sizeof(why));
        err = std::string("converting the frame to RGB failed") + (r < 0 ? ": " : "") + why;
        out = {};
        return false;
    }
    return true;
}

void FisheyeDemux::keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const
{
    m_demux.keyframesAroundMs(timestampMs, before, after);
}

int64_t FisheyeDemux::nearestKeyframeMs(int64_t timestampMs) const
{
    return m_demux.nearestKeyframeMs(timestampMs);
}

bool FisheyeDemux::seekTo(int64_t timestampMs, std::string& err)
{
    return m_demux.seekTo(timestampMs, err);
}

bool FisheyeDemux::readNextPair(Image& out0, Image& out1, FramePts* pts, std::string& err)
{
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    if (!m_demux.readNextPair(f0, f1, pts, err))
        return false;
    bool ok = convert(f0, lens(0), out0, err);
    if (ok && f1->data[0] == f0->data[0]) // both lenses in one frame: converted once
        out1 = out0;
    else if (ok)
        ok = convert(f1, lens(1), out1, err);
    av_frame_free(&f0);
    av_frame_free(&f1);
    if (!ok) { // neither lens's picture: no half pair
        out0 = {};
        out1 = {};
        return false;
    }
    crop(0, out0);
    crop(1, out1);
    return true;
}

bool FisheyeDemux::readFrameAt(int lens, int64_t timestampMs, Image& out, std::string& err)
{
    if (lens < 0 || lens >= lensCount()) {
        err = "lens index " + std::to_string(lens) + " out of range";
        return false;
    }
    if (!seekTo(timestampMs, err))
        return false;
    if (lensCount() < 2) {
        // Single-stream fallback: decode just this lens forward from the seek.
        AVFrame* f = nullptr;
        if (!m_demux.readNextFrame(lens, f, err)) {
            if (err.empty())
                err = "no frame decoded for lens " + std::to_string(lens);
            return false;
        }
        const bool ok = convert(f, this->lens(lens), out, err);
        av_frame_free(&f);
        return ok;
    }
    Image a, b;
    if (!readNextPair(a, b, nullptr, err)) {
        if (err.empty())
            err = "no frame decoded for lens " + std::to_string(lens) + " at " +
                  std::to_string(timestampMs) + " ms";
        return false;
    }
    out = std::move(lens == 0 ? a : b);
    return true;
}

} // namespace orbiscore
