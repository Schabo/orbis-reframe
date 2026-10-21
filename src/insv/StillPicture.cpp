// SPDX-License-Identifier: MIT OR Apache-2.0
#include "StillPicture.h"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace insv {

bool DecodeStillPicture(const std::vector<uint8_t>& bitstream, const char* codec, int maxSide,
                        Image& out, std::string& err)
{
    const AVCodec* dec = avcodec_find_decoder_by_name(codec);
    if (!dec) {
        err = std::string("no ") + codec + " decoder";
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    bool ok = false;
    if (ctx && pkt && frm && avcodec_open2(ctx, dec, nullptr) >= 0) {
        // One packet with the whole picture, then the flush: the decoder
        // hands it out on either.
        pkt->data = const_cast<uint8_t*>(bitstream.data());
        pkt->size = static_cast<int>(bitstream.size());
        avcodec_send_packet(ctx, pkt);
        avcodec_send_packet(ctx, nullptr);
        ok = avcodec_receive_frame(ctx, frm) == 0;
        if (!ok)
            err = std::string("no picture decoded from the ") + codec + " frame";
    } else {
        err = std::string("can't open the ") + codec + " decoder";
    }
    if (ok) {
        const double s = std::min(1.0, static_cast<double>(maxSide) / std::max(frm->width, frm->height));
        const int w = std::max(2, static_cast<int>(frm->width * s) & ~1);
        const int h = std::max(2, static_cast<int>(frm->height * s) & ~1);
        // The deprecated full-range (yuvj) formats: the plain format, with the
        // range set apart (as InsvDemux does).
        auto fmt = static_cast<AVPixelFormat>(frm->format);
        bool full = frm->color_range == AVCOL_RANGE_JPEG;
        switch (fmt) {
        case AV_PIX_FMT_YUVJ420P: fmt = AV_PIX_FMT_YUV420P; full = true; break;
        case AV_PIX_FMT_YUVJ422P: fmt = AV_PIX_FMT_YUV422P; full = true; break;
        case AV_PIX_FMT_YUVJ444P: fmt = AV_PIX_FMT_YUV444P; full = true; break;
        default: break;
        }
        const int coef = frm->colorspace == AVCOL_SPC_BT709        ? SWS_CS_ITU709
                         : frm->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020
                                                                   : SWS_CS_ITU601;
        SwsContext* sws = sws_getContext(frm->width, frm->height, fmt, w, h, AV_PIX_FMT_RGB24,
                                         SWS_AREA, nullptr, nullptr, nullptr);
        if (sws) {
            const int* c = sws_getCoefficients(coef);
            sws_setColorspaceDetails(sws, c, full ? 1 : 0, c, 1, 0, 1 << 16, 1 << 16);
            out.alloc(w, h);
            uint8_t* dst[1] = {out.rgb.data()};
            int stride[1] = {w * 3};
            sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dst, stride);
            sws_freeContext(sws);
        } else {
            ok = false;
            err = "can't convert the decoded picture";
        }
    }
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return ok;
}

} // namespace insv
