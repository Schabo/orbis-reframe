#include "ReframeExportController.h"

#include <QFileInfo>
#include <QStringList>

#include "ReframeFlatten.h"  // exportpipe::FlattenWriter (system FFmpeg)
#include "ReframeRenderer.h" // exportpipe::RenderRectilinear

#include <ClipMetadata.h>       // camera::ReadClipImu
#include <NativeStitchSource.h> // stitch::NativeStitchSource, stitch::Frame, stitch::PreviewStitchConfig
#include <ProbeClip.h>          // stitch::ProbeClip

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kAudioRate = 48000; // the X5's own rate -- no resampling in practice
constexpr int kAudioChannels = 2;

} // namespace

stitch::CaptureTiming
ReframeExportController::captureTimingFor(std::shared_ptr<const insv::GyroStabilizer> stab)
{
    stitch::CaptureTiming t;
    if (!stab || !stab->ready())
        return t;
    t.readoutSec[0] = t.readoutSec[1] = stab->readoutSec();
    t.rotationAt = [stab](double tSec, double dtSec, float r[3]) {
        stab->bodyRotation(tSec, dtSec, r);
    };
    return t;
}

std::shared_ptr<const insv::GyroStabilizer>
ReframeExportController::buildStabilizer(const QString &path, const insv::StabConfig &config,
                                         double syncMs)
{
    const auto imu = camera::ReadClipImu(path.toStdString());
    if (!imu)
        return nullptr;
    auto stab = std::make_shared<insv::GyroStabilizer>();
    stab->build(*imu, config, syncMs / 1000.0);
    return stab->ready() ? stab : nullptr;
}

uint32_t ReframeExportController::fillRgba(const pane::Split &split)
{
    const uint32_t rgb = split.gapRgb;
    return ((rgb >> 16) & 0xFFu) | (rgb & 0xFF00u) | ((rgb & 0xFFu) << 16) | 0xFF000000u;
}

void ReframeExportController::applyPanes(stitch::NativeStitchSource &src, const ClipJob &c,
                                         int outW, int outH)
{
    std::vector<stitch::NativeStitchSource::ViewPane> panes;
    if (c.split.isSplit())
        for (const pane::Rect &r : pane::rects(c.split, outW, outH))
            panes.push_back({r.x, r.y, r.w, r.h});
    src.SetViewPanes(std::move(panes),
                     [fn = c.panePoseAt](int64_t ptsMs, int i, view::Pose &pose) {
                         if (fn)
                             fn(ptsMs, i, pose);
                     },
                     fillRgba(c.split));
}

std::vector<exportpipe::PaneView> ReframeExportController::paneViews(const ClipJob &c, int outW,
                                                                     int outH, int64_t ptsMs,
                                                                     const view::Pose &pose0)
{
    std::vector<exportpipe::PaneView> views;
    if (!c.split.isSplit())
        return views;
    for (const pane::Rect &r : pane::rects(c.split, outW, outH)) {
        exportpipe::PaneView v;
        v.x = r.x;
        v.y = r.y;
        v.w = r.w;
        v.h = r.h;
        if (views.empty())
            v.pose = pose0;
        else if (c.panePoseAt)
            c.panePoseAt(ptsMs, static_cast<int>(views.size()), v.pose);
        views.push_back(v);
    }
    return views;
}

QImage ReframeExportController::renderStill(const ClipJob &c, qint64 sourceMs,
                                            const view::Pose &pose, int outW, int outH,
                                            QString &error)
{
    if (outW <= 0 || outH <= 0) {
        error = tr("Invalid output size.");
        return {};
    }
    if (c.flat) { // the picture is the caller's inset layer
        QImage black(outW, outH, QImage::Format_RGB888);
        black.fill(Qt::black);
        return black;
    }
    stitch::ClipProbe probe;
    std::string perr;
    if (!stitch::ProbeClip(c.clipPath.toStdString(), probe, perr))
        probe = stitch::ClipProbe{};
    stitch::PreviewStitchConfig sc;
    sc.width = stitch::ExportStitchWidth(probe.width, outW) & ~1;
    sc.height = (sc.width / 2) & ~1;
    sc.lensGuard = c.lensGuard;
    sc.depthSeam = c.depthSeam;

    stitch::NativeStitchSource src;
    if (!src.Open(c.clipPath.toStdString(), sc) || !src.SeekTo(sourceMs)) {
        error = QString::fromStdString(src.GetLastError());
        return {};
    }
    src.SetCaptureTiming(captureTimingFor(c.stab));
    src.SetSeamCorrection(c.seamCorrection);
    if (c.blur && !src.SetBlurQuads(c.blur)) {
        error = tr("This stitch can't apply the privacy blur.");
        return {};
    }
    applyPanes(src, c, outW, outH);

    // Stab at the decoded frame's own time, as the export does.
    auto stabAt = [&c](int64_t ptsMs, float stab[9]) {
        if (c.stab) {
            float m[3][3];
            c.stab->correctionMatrix(ptsMs / 1000.0, m);
            std::copy(&m[0][0], &m[0][0] + 9, stab);
        }
    };

    // The fused GPU pass (straight from the fisheyes, one resample)
    // when there is one -- needs outW % 4 == 0, as every preset is.
    if (src.GpuViewAvailable() && outW % 4 == 0 && outH % 2 == 0) {
        const stitch::Frame f = src.ReadViewFrame(
            outW, outH,
            [&](int64_t ptsMs, view::Pose &p, float stab[9]) {
                p = pose;
                stabAt(ptsMs, stab);
            },
            &c.colorGrade);
        if (f.rgba.empty()) {
            error = src.GetLastError().empty() ? tr("No frame at the playhead.")
                                               : QString::fromStdString(src.GetLastError());
            return {};
        }
        const QImage view(f.rgba.data(), outW, outH, outW * 4, QImage::Format_RGBA8888);
        return view.convertToFormat(QImage::Format_RGB888); // deep copy; alpha is 255
    }

    // Else: the equirect, reprojected on the CPU.
    const stitch::Frame eq = src.ReadFrame();
    if (eq.rgba.empty()) {
        error = src.GetLastError().empty() ? tr("No frame at the playhead.")
                                           : QString::fromStdString(src.GetLastError());
        return {};
    }

    exportpipe::ReframeView view;
    view.pose = pose;
    view.aspect = static_cast<float>(outW) / outH;
    stabAt(eq.timestamp_ms, view.stab);
    view.colorGrade = c.colorGrade;

    QImage img(outW, outH, QImage::Format_RGBA8888);
    const std::vector<exportpipe::PaneView> panes =
        paneViews(c, outW, outH, eq.timestamp_ms, pose);
    if (panes.empty())
        exportpipe::RenderRectilinear(eq.rgba.data(), eq.width, eq.height, img.bits(), outW, outH,
                                      view);
    else
        exportpipe::RenderRectilinearPanes(eq.rgba.data(), eq.width, eq.height, img.bits(), outW,
                                           outH, panes, fillRgba(c.split), view);
    return img.convertToFormat(QImage::Format_RGB888); // opaque: alpha is always 255
}

ReframeExportController::ReframeExportController(QObject *parent)
    : QObject(parent)
{
}

void ReframeExportController::run(const Request &req)
{
    m_cancel.store(false, std::memory_order_relaxed);

    if (req.clips.isEmpty()) {
        emit finished(false, tr("The timeline is empty."), req.outPath);
        return;
    }

    // Which output frames each clip fills: those whose timeline time
    // (llround(k * outFrameMs), as the overlays and transitions are drawn
    // at) falls in its span on the timeline. So the cut lands on the same
    // frame for the main track as for a transition's layers over it -- a
    // frame apart, one clip would show at full strength in the middle of a
    // crossfade -- and it doesn't drift by rounding each clip's length on
    // its own. Also each clip's weight for the one progress bar.
    const double outFrameMs = 1000.0 / req.fps;
    const auto frameAtOrAfter = [outFrameMs](int64_t ms) {
        int64_t k = std::max<int64_t>(0, static_cast<int64_t>(std::ceil(ms / outFrameMs)));
        while (k > 0 && std::llround((k - 1) * outFrameMs) >= ms)
            --k;
        while (std::llround(k * outFrameMs) < ms)
            ++k;
        return k;
    };
    const int n = req.clips.size();
    std::vector<int64_t> frames(n), firstFrame(n), clipStartMs(n);
    int64_t totalFrames = 0;
    int64_t startMs = 0;
    for (int i = 0; i < n; ++i) {
        const ClipJob &c = req.clips[i];
        // Its span on the timeline, at its speed (ClipItem::timelineDurationMs).
        const int64_t endMs = startMs + std::llround((c.trimOutMs - c.trimInMs) / c.speed);
        clipStartMs[i] = startMs;
        firstFrame[i] = frameAtOrAfter(startMs);
        frames[i] = std::max<int64_t>(0, frameAtOrAfter(endMs) - firstFrame[i]);
        totalFrames += frames[i];
        startMs = endMs;
    }
    if (totalFrames <= 0)
        totalFrames = 1;

    auto emitPct = [&](double pct, const QString &label) {
        emit progress(std::clamp(static_cast<int>(std::lround(pct)), 0, 100), label);
    };


    exportpipe::FlattenConfig cfg;
    cfg.out_path = req.outPath.toStdString();
    cfg.out_w = req.outWidth;
    cfg.out_h = req.outHeight;
    cfg.fps = req.fps;
    cfg.h265 = req.h265;
    cfg.ten_bit = req.tenBit;
    cfg.bitrate_kbps = req.bitrateKbps;
    // An audio track only if there's something on the audio tracks.
    const bool withAudio = !req.mix.empty();
    insv::AudioMixer mixer(kAudioRate, kAudioChannels);
    mixer.setClips(req.mix);
    if (withAudio) {
        cfg.audio_sample_rate = kAudioRate;
        cfg.audio_channels = kAudioChannels;
    }

    std::string err;

    // Held by pointer so a cancelled export can close (destroy) the encoder and
    // its half-written output file *before* deleting that file below.
    auto writer = std::make_unique<exportpipe::FlattenWriter>();
    int64_t outFrames = 0; // video frames sent to the encoder (the one being encoded included)
    // The hardware encoder failing is said in words -- its own errors
    // ("Function not implemented") don't point at the usual cause -- with
    // them as the details.
    auto failed = [&](const QString &e) {
        if (!writer->encoderFailed()) {
            emit finished(false, e, req.outPath);
            return;
        }
        const QString why =
            outFrames > 0
                ? tr("The graphics card's video encoder stopped at frame %1 of %2, most "
                     "often because the graphics memory is full.")
                      .arg(outFrames)
                      .arg(totalFrames)
                : tr("The graphics card's video encoder couldn't start, most often because the "
                     "graphics memory is full.");
        emit finished(false,
                      why + QLatin1Char(' ') +
                          tr("Close other programs that use the graphics card (another "
                             "orbis-reframe, games, a web browser) and export again."),
                      req.outPath, e);
    };
    if (!writer->open(cfg, err)) {
        failed(QString::fromStdString(err));
        return;
    }
    // Overlays: output frame k is timeline time k / fps (the audio mix's
    // clock too).
    auto insets = std::make_shared<exportpipe::InsetCompositor>(req.insets, req.outWidth,
                                                                req.outHeight);
    if (!insets->empty())
        writer->setFrameHook([insets, outFrameMs](int64_t k, uint8_t *y, int ys, uint8_t *uv,
                                                  int uvs, bool p010, std::string &e) {
            return insets->composite(std::llround(k * outFrameMs), y, ys, uv, uvs, e, p010);
        });
    // Said in the "Export complete" message: a 10-bit export that is 8-bit
    // after all, or has 360 clips that only got 8 bits (no fused GPU pass).
    QString note;
    if (cfg.ten_bit && !writer->tenBit())
        note = tr("Exported as 8-bit: the video encoder couldn't encode 10-bit HEVC (%1).")
                   .arg(QString::fromStdString(writer->tenBitFallback()));
    QStringList eightBitClips;
    // ...and clips that went through the CPU because the GPU part didn't
    // start: the picture is the same, but the export took much longer.
    QStringList cpuClips;

    bool cancelled = false;
    int64_t framesBefore = 0;
    std::vector<float> pcm;
    std::vector<uint8_t> black; // a flat clip's frames, NV12 (BT.709 limited range)
    for (int i = 0; i < n && !cancelled; ++i) {
        const ClipJob &c = req.clips[i];
        const double base = framesBefore * 100.0 / totalFrames;
        const double span = frames[i] * 100.0 / totalFrames;
        const QString label = tr("Rendering clip %1 of %2…").arg(i + 1).arg(n);
        // Per-clip failures name the clip so a mid-export error is traceable
        // back to the timeline.
        auto failClip = [&](const QString &e) {
            failed(tr("Clip %1 of %2 (%3): %4")
                       .arg(i + 1)
                       .arg(n)
                       .arg(QFileInfo(c.clipPath).fileName(), e));
        };

        auto feedAudio = [&]() -> bool {
            if (!withAudio)
                return true;
            // Audio up to the end of output frame `outFrames` on the timeline.
            const int64_t target = std::llround(outFrames * kAudioRate / req.fps);
            const int64_t need = target - writer->audioFramesWritten();
            if (need <= 0)
                return true;
            pcm.assign(static_cast<size_t>(need) * kAudioChannels, 0.0f);
            mixer.mixInto(pcm.data(), writer->audioFramesWritten(), need); // the audio tracks
            insv::AudioMixer::limit(pcm.data(), need * kAudioChannels);
            return writer->addAudio(pcm.data(), need, err);
        };

        if (c.flat) {
            // Black frames for its span; the frame hook draws the clip (one of
            // the insets) over each at its timeline time.
            black.resize(size_t(req.outWidth) * req.outHeight * 3 / 2);
            std::fill(black.begin(), black.begin() + size_t(req.outWidth) * req.outHeight, 16);
            std::fill(black.begin() + size_t(req.outWidth) * req.outHeight, black.end(), 128);
            for (int64_t k = 0; k < frames[i]; ++k) {
                if (m_cancel.load(std::memory_order_relaxed)) {
                    cancelled = true;
                    break;
                }
                ++outFrames;
                if (!writer->addNv12Frame(black.data(), req.outWidth, req.outHeight, err) ||
                    !feedAudio()) {
                    failClip(QString::fromStdString(err));
                    return;
                }
                emitPct(base + span * static_cast<double>(k + 1) / frames[i], label);
            }
            framesBefore += frames[i];
            continue;
        }

        // Equirect intermediate size, 2:1: per clip, the full source sphere (and
        // at least 2x the output width), so the export keeps all the detail
        // the camera recorded (stitch::ExportStitchWidth).
        stitch::ClipProbe probe;
        std::string perr;
        if (!stitch::ProbeClip(c.clipPath.toStdString(), probe, perr))
            probe = stitch::ClipProbe{};
        int eqW = req.stitchWidth;
        if (eqW <= 0)
            eqW = stitch::ExportStitchWidth(probe.width, req.outWidth);
        // Conform: output frame k of this clip shows the source frame whose
        // time window [pts - half, pts + half) holds the source time of its
        // timeline time. A faster source (or speed > 1) drops frames, a
        // slower one (or slow motion) repeats them.
        const double srcHalfMs = 500.0 / probe.fps;
        auto targetSrcMs = [&](int64_t k) {
            const int64_t ms = std::llround((firstFrame[i] + k) * outFrameMs);
            return c.trimInMs + double(ms - clipStartMs[i]) * c.speed;
        };
        eqW &= ~1;
        stitch::PreviewStitchConfig sc;
        sc.width = eqW;
        sc.height = (req.stitchHeight > 0 ? req.stitchHeight : eqW / 2) & ~1;
        sc.lensGuard = c.lensGuard;
        sc.depthSeam = c.depthSeam;

        auto src = std::make_unique<stitch::NativeStitchSource>();
        if (!src->Open(c.clipPath.toStdString(), sc) || !src->SeekTo(c.trimInMs)) {
            failClip(QString::fromStdString(src->GetLastError()));
            return;
        }
        src->SetCaptureTiming(captureTimingFor(c.stab));
        src->SetSeamCorrection(c.seamCorrection);
        src->SetFlatTenBit(writer->tenBit());
        applyPanes(*src, c, req.outWidth, req.outHeight);
        if (c.blur && !src->SetBlurQuads(c.blur)) {
            failClip(tr("This stitch can't apply the privacy blur."));
            return;
        }
        // Into a 10-bit file, a clip whose frames come as 8-bit (the CPU
        // path, or the GPU equirect) is named in the note; checked on its
        // first frame.
        bool depthChecked = !writer->tenBit();
        auto checkDepth = [&](bool p010) {
            if (depthChecked)
                return;
            depthChecked = true;
            if (!p010)
                eightBitClips << QFileInfo(c.clipPath).fileName();
        };

        // GPU reproject straight to NV12 / P010 -> the encoder when the
        // all-GPU stitch path is active (no CPU equirect->rectilinear pass,
        // no RGBA->YUV swscale).
        // Needs outW % 4 == 0 and outH % 2 == 0.
        const bool gpuFlat = src->GpuReframeAvailable() && (req.outWidth % 4) == 0 &&
                             (req.outHeight % 2) == 0;

        // Pose + stab for a decoded frame, at that frame's source time.
        auto poseAt = [&c](int64_t ptsMs, view::Pose &pose, float stab[9]) {
            if (c.poseAt)
                c.poseAt(ptsMs, pose);
            if (c.stab) {
                float m[3][3];
                c.stab->correctionMatrix(ptsMs / 1000.0, m);
                std::copy(&m[0][0], &m[0][0] + 9, stab);
            }
        };

        int64_t written = 0;
        // How many of this clip's next output frames the source frame at
        // `ptsMs` stands for (0 = dropped).
        auto repeatsFor = [&](int64_t ptsMs) {
            int64_t n = 0;
            while (written + n < frames[i] && targetSrcMs(written + n) < ptsMs + srcHalfMs)
                ++n;
            return n;
        };
        while (written < frames[i]) {
            if (m_cancel.load(std::memory_order_relaxed)) {
                cancelled = true;
                break;
            }

            if (gpuFlat) {
                stitch::FlatFrame ff = src->ReadFlatFrame(req.outWidth, req.outHeight, poseAt, &c.colorGrade);
                if (ff.data.empty()) { // clean end-of-clip, or error
                    const std::string &e = src->GetLastError();
                    if (!e.empty()) {
                        failClip(QString::fromStdString(e));
                        return;
                    }
                    break;
                }
                if (ff.timestamp_ms + srcHalfMs < c.trimInMs)
                    continue; // straggler ahead of the in-point
                const int64_t reps = repeatsFor(ff.timestamp_ms);
                checkDepth(ff.p010);
                for (int64_t r = 0; r < reps; ++r) {
                    ++outFrames;
                    const bool added =
                        ff.p010 ? writer->addP010Frame(ff.data.data(), ff.width, ff.height, err)
                                : writer->addNv12Frame(ff.data.data(), ff.width, ff.height, err);
                    if (!added || !feedAudio()) {
                        failClip(QString::fromStdString(err));
                        return;
                    }
                }
                written += reps;
            } else {
                stitch::Frame eq = src->ReadFrame();
                if (eq.rgba.empty()) { // clean end-of-clip, or error
                    const std::string &e = src->GetLastError();
                    if (!e.empty()) {
                        failClip(QString::fromStdString(e));
                        return;
                    }
                    break;
                }
                // NativeStitchSource::SeekTo already lands at-or-after trimInMs;
                // drop any straggler still ahead of the in-point just in case.
                if (eq.timestamp_ms + srcHalfMs < c.trimInMs)
                    continue;
                const int64_t reps = repeatsFor(eq.timestamp_ms);
                if (reps == 0)
                    continue; // dropped: skip the CPU reproject too
                checkDepth(false);
                view::Pose pose;
                float stab[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                poseAt(eq.timestamp_ms, pose, stab);
                const std::vector<exportpipe::PaneView> panes =
                    paneViews(c, req.outWidth, req.outHeight, eq.timestamp_ms, pose);
                for (int64_t r = 0; r < reps; ++r) {
                    ++outFrames;
                    const bool added =
                        panes.empty()
                            ? writer->addEquirectFrame(eq.rgba.data(), eq.width, eq.height,
                                                       c.poseAt ? &pose : nullptr,
                                                       c.stab ? stab : nullptr, &c.colorGrade, err)
                            : writer->addEquirectPanes(eq.rgba.data(), eq.width, eq.height, panes,
                                                       fillRgba(c.split), c.stab ? stab : nullptr,
                                                       &c.colorGrade, err);
                    if (!added || !feedAudio()) {
                        failClip(QString::fromStdString(err));
                        return;
                    }
                }
                written += reps;
            }
            if (frames[i] > 0)
                emitPct(base + span * static_cast<double>(written) / frames[i], label);
        }
        // After the frames: the GPU stitch can also give up part-way.
        if (src->GetGpuFallback().any())
            cpuClips << QFileInfo(c.clipPath).fileName();
        framesBefore += frames[i];
    }

    if (cancelled) {
        writer.reset(); // close the encoder + file before removing it
        std::remove(cfg.out_path.c_str());
        // Empty error string on !ok is the "cancelled by the user" signal.
        emit finished(false, QString(), req.outPath);
        return;
    }

    const int64_t total = writer->finish(err);
    if (total < 0) {
        failed(QString::fromStdString(err));
        return;
    }
    emitPct(100, tr("Done"));
    eightBitClips.removeDuplicates(); // a file used twice on the timeline
    if (!eightBitClips.isEmpty())
        note = tr("These clips were exported with 8-bit precision inside the 10-bit file "
                  "(the GPU stitch isn't available for them): %1")
                   .arg(eightBitClips.join(QStringLiteral(", ")));
    cpuClips.removeDuplicates();
    if (!cpuClips.isEmpty()) {
        if (!note.isEmpty())
            note += QStringLiteral("\n\n");
        note += tr("The graphics card couldn't decode or stitch these clips, most often because "
                   "its memory was full, so they were done on the processor, more slowly: %1")
                    .arg(cpuClips.join(QStringLiteral(", ")));
    }
    emit finished(true, note, req.outPath);
}
