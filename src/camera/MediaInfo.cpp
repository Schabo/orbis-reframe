#include "MediaInfo.h"

#include "ClipMetadata.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

#include <filesystem>
#include <system_error>

namespace camera {
namespace {

std::string profileName(const AVCodecParameters* p) {
    const char* name = avcodec_profile_name(p->codec_id, p->profile);
    return name ? name : "";
}

std::string tag(AVDictionary* dict, const char* key) {
    const AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0);
    return e && e->value ? e->value : "";
}

} // namespace

bool ProbeMediaInfo(const std::string& path, MediaInfo& out, std::string& err) {
    out = MediaInfo{};
    out.path = path;
    std::error_code ec;
    out.fileBytes = static_cast<int64_t>(std::filesystem::file_size(path, ec));
    if (ec) {
        err = std::filesystem::exists(path, ec) ? "cannot read the file" : "file not found";
        return false;
    }

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        err = "not a readable media file";
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        err = "cannot read the stream info";
        return false;
    }
    out.container = fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "";
    out.durationMs = fmt->duration > 0 ? fmt->duration / (AV_TIME_BASE / 1000) : 0;
    out.totalBitRate = fmt->bit_rate;
    out.creationTime = tag(fmt->metadata, "creation_time");

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* p = st->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            MediaVideoStream v;
            v.codec = avcodec_get_name(p->codec_id);
            v.profile = profileName(p);
            v.width = p->width;
            v.height = p->height;
            const AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
            v.fps = r.den ? av_q2d(r) : 0.0;
            v.bitRate = p->bit_rate;
            const auto pf = static_cast<AVPixelFormat>(p->format);
            if (const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf)) {
                v.pixelFormat = d->name;
                v.bitDepth = d->comp[0].depth;
                if (!(d->flags & AV_PIX_FMT_FLAG_RGB) && d->nb_components >= 3)
                    v.chroma = d->log2_chroma_w == 1 ? (d->log2_chroma_h == 1 ? "4:2:0" : "4:2:2")
                                                     : "4:4:4";
            }
            // yuvj* formats are full range by definition, whatever color_range says.
            v.fullRange = p->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P ||
                          pf == AV_PIX_FMT_YUVJ422P || pf == AV_PIX_FMT_YUVJ444P;
            if (p->color_space != AVCOL_SPC_UNSPECIFIED)
                v.colorSpace = av_color_space_name(p->color_space);
            v.frameCount = st->nb_frames;
            out.video.push_back(v);
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
            MediaAudioStream a;
            a.codec = avcodec_get_name(p->codec_id);
            a.profile = profileName(p);
            a.sampleRate = p->sample_rate;
            a.channels = p->ch_layout.nb_channels;
            char layout[64] = {};
            if (av_channel_layout_describe(&p->ch_layout, layout, sizeof layout) > 0)
                a.channelLayout = layout;
            a.bitRate = p->bit_rate;
            out.audio.push_back(a);
        }
    }
    avformat_close_input(&fmt);

    // Camera facts for a clip a camera reader knows; a plain video has none.
    if (const auto clip = ReadClipMetadata(path)) {
        out.cameraModel = clip->model;
        out.serialNumber = clip->serial;
        out.firmware = clip->firmware;
        out.imuRateHz = clip->imuRateHz;
        out.exposureRecords = clip->exposureRecords;
        out.rollingShutterMs = clip->rollingShutterMs;
        out.colorProfile = clip->colorProfile;
        out.logProfile = clip->logProfile;
        out.gpsRecords = clip->gpsRecords;
        out.gpsFixes = clip->gpsFixes;
        out.gpsRateHz = clip->gpsRateHz;
        out.gpsTrackSec = clip->gpsTrackSec;
        out.proxyPath = clip->proxyPath;
        out.captureMode = clip->captureMode;
    }
    return true;
}

} // namespace camera
