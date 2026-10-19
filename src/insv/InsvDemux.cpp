// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvDemux.h"

#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace insv {

InsvDemux::~InsvDemux() { close(); }

void InsvDemux::close()
{
    m_demux.close();
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_swsSrcFullRange = -1;
}

bool InsvDemux::open(const std::string& path, std::string& err)
{
    close();
    return m_demux.open(
        path,
        [](const AVStream*, const AVCodec*, AVCodecContext* ctx, std::string&) {
            ctx->thread_count = 0; // let FFmpeg pick; frame+slice threading
            return true;
        },
        err);
}

void InsvDemux::convert(const AVFrame* frm, const InsvStreamInfo& stream, Image& out)
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
    m_sws = sws_getCachedContext(m_sws, frm->width, frm->height, srcFmt, frm->width, frm->height,
                                 AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (m_sws && (m_sws != prev || srcFullRange != m_swsSrcFullRange || coefId != m_swsCoefId)) {
        const int* coef = sws_getCoefficients(coefId);
        // src range = srcFullRange, dst (RGB) range = full; neutral bright/contrast/sat.
        sws_setColorspaceDetails(m_sws, coef, srcFullRange, coef, 1, 0, 1 << 16, 1 << 16);
        m_swsSrcFullRange = srcFullRange;
        m_swsCoefId = coefId;
    }

    uint8_t* dst[1] = {out.rgb.data()};
    int stride[1] = {out.w * 3};
    sws_scale(m_sws, frm->data, frm->linesize, 0, frm->height, dst, stride);
}

void InsvDemux::keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const
{
    m_demux.keyframesAroundMs(timestampMs, before, after);
}

int64_t InsvDemux::nearestKeyframeMs(int64_t timestampMs) const
{
    return m_demux.nearestKeyframeMs(timestampMs);
}

bool InsvDemux::seekTo(int64_t timestampMs, std::string& err) { return m_demux.seekTo(timestampMs, err); }

bool InsvDemux::readNextPair(Image& out0, Image& out1, int64_t* ptsMs, std::string& err)
{
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    if (!m_demux.readNextPair(f0, f1, ptsMs, err))
        return false;
    convert(f0, lens(0), out0);
    convert(f1, lens(1), out1);
    av_frame_free(&f0);
    av_frame_free(&f1);
    return true;
}

bool InsvDemux::readFrameAt(int lens, int64_t timestampMs, Image& out, std::string& err)
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
        convert(f, this->lens(lens), out);
        av_frame_free(&f);
        return true;
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

} // namespace insv
