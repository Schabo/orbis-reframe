#include "MediaProbe.h"

#include <ClipMetadata.h>    // camera::ReadClipMetadata
#include <FlatVideoReader.h> // exportpipe::FlatVideoReader
#include <InsvAudioDemux.h>  // insv::InsvAudioDemux
#include <ProbeClip.h>       // stitch::ProbeClip

#include <algorithm>

#include "ClipThumbnail.h"

std::optional<MediaItem> probeMedia(const QString &path, QString *error,
                                    const ClipProbeFn &probeClip)
{
    const auto kind = media::kindForPath(path);
    if (!kind) {
        if (error)
            *error = QObject::tr("not a video or audio file");
        return std::nullopt;
    }
    MediaItem m;
    m.path = path;
    m.kind = *kind;
    std::string err;
    bool ok = false;
    switch (*kind) {
    case MediaItem::Kind::Clip360: {
        stitch::ClipProbe probe;
        ok = probeClip ? probeClip(path.toStdString(), probe, err)
                       : stitch::ProbeClip(path.toStdString(), probe, err);
        m.durationMs = probe.duration_ms;
        m.width = probe.width;
        m.height = probe.height;
        m.fps = probe.fps;
        // Another file of a take (a ONE X2's second lens or proxy): the clip
        // is the take's.
        if (!probe.clip_path.empty())
            m.path = QString::fromStdString(probe.clip_path);
        break;
    }
    case MediaItem::Kind::Video: {
        exportpipe::FlatVideoReader::Info info;
        ok = exportpipe::FlatVideoReader::probe(path.toStdString(), info, err);
        m.durationMs = info.durationMs;
        m.width = info.width;
        m.height = info.height;
        m.fps = info.fps;
        break;
    }
    case MediaItem::Kind::Audio: {
        insv::InsvAudioDemux demux;
        ok = demux.open(path.toStdString(), 48000, 2, err);
        m.durationMs = demux.durationMs();
        if (ok && m.durationMs <= 0) {
            ok = false;
            err = "no audio";
        }
        break;
    }
    }
    if (!ok) {
        if (error)
            *error = err.empty() ? QObject::tr("can't be read") : QString::fromStdString(err);
        return std::nullopt;
    }
    return m;
}

QImage mediaThumbnail(const MediaItem &item, const QSize &size)
{
    if (size.isEmpty())
        return {};
    switch (item.kind) {
    case MediaItem::Kind::Clip360:
        return clipThumbnail(item.path, size);
    case MediaItem::Kind::Video:
        break;
    case MediaItem::Kind::Audio:
        return {};
    }

    exportpipe::FlatVideoReader reader;
    std::string err;
    if (!reader.open(item.path.toStdString(), err))
        return {};
    // Decode straight to the thumbnail's width (fewer bytes to convert), at
    // the video's shape; cropped to the thumbnail's below.
    const auto &info = reader.info();
    if (info.width <= 0 || info.height <= 0)
        return {};
    const double scale = std::max(double(size.width()) / info.width,
                                  double(size.height()) / info.height);
    reader.setOutputSize(std::max(2, int(info.width * scale + 0.5)) & ~1,
                         std::max(2, int(info.height * scale + 0.5)) & ~1);
    const int64_t at = info.durationMs > 2000 ? 1000 : info.durationMs / 2;
    const exportpipe::VideoFrame *frame = reader.frameAt(at);
    if (!frame || frame->rgba.empty())
        return {};
    const QImage full(frame->rgba.data(), frame->width, frame->height, frame->width * 4,
                      QImage::Format_RGBA8888);
    const QRect crop(QPoint((full.width() - size.width()) / 2, (full.height() - size.height()) / 2),
                     size);
    return full.copy(crop.intersected(full.rect()))
        .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QString mediaMode(const MediaItem &item)
{
    if (item.kind != MediaItem::Kind::Clip360)
        return {};
    const auto clip = camera::ReadClipMetadata(item.path.toStdString());
    return clip ? QString::fromStdString(clip->captureMode) : QString();
}
