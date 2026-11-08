#include "NativeStitchSource.h"

#include "../core/Log.h"
#include "ClipSeamAnalysis.h"

#include <Calibration.h>           // insv::InsvCalibration
#include <FisheyeProjection.h>     // insv::FisheyeStitcher, insv::StitchParams
#include <Image.h>                 // insv::Image
#include <InsvDemux.h>             // insv::InsvDemux
#include <SeamFlow.h>              // insv::SeamFlowStrips, MeasureSeamFlowStrips
#ifdef ORBIS_SEAM_FLOW
#include <SeamFlowEstimator.h>     // insv::SeamFlowEstimator (OpenCV)
#endif
#include <VulkanFisheyeStitcher.h> // render::VulkanFisheyeStitcher
#include <VulkanVideoDecoder.h>    // insv::VulkanVideoDecoder (stage 2, GPU decode)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>

namespace stitch {

namespace {

// RGB24 (insv::Image) -> interleaved RGBA (stitch::Frame), opaque alpha.
Frame ToRgbaFrame(const insv::Image& eq, const insv::FramePts& pts) {
    Frame f;
    f.width = eq.w;
    f.height = eq.h;
    f.timestamp_ms = pts.ms;
    f.timestamp_us = pts.us;
    const size_t n = static_cast<size_t>(eq.w) * static_cast<size_t>(eq.h);
    f.rgba.resize(n * 4);
    const uint8_t* src = eq.rgb.data();
    uint8_t* dst = f.rgba.data();
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = src[i * 3 + 0];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
    }
    return f;
}

render::VulkanLensInput toRenderInput(const insv::VulkanLensFrame& lf) {
    render::VulkanLensInput in;
    in.image = lf.image;
    in.format = lf.format;
    in.layout = lf.layout;
    in.semaphore = lf.semaphore;
    in.semWaitValue = lf.semWaitValue;
    in.width = lf.width;
    in.height = lf.height;
    in.usage = lf.usage;
    return in;
}

} // namespace

const insv::LensProfile* LensGuardProfile(const camera::ClipMetadata& clip,
                                          PreviewStitchConfig::LensGuard guard) {
    const std::optional<insv::LensProfile>* p = nullptr;
    switch (guard) {
        case PreviewStitchConfig::LensGuard::Standard: p = &clip.standardGuard; break;
        case PreviewStitchConfig::LensGuard::Premium: p = &clip.premiumGuard; break;
        case PreviewStitchConfig::LensGuard::None: break;
    }
    return p && *p ? &**p : nullptr;
}

bool DualFisheyeStitchSetup(const camera::ClipMetadata& clip,
                            PreviewStitchConfig::LensGuard guard,
                            const LensCalibration* calibration, bool modelCorrection,
                            insv::InsvCalibration& calib, insv::StitchParams& params,
                            std::string& err, bool* guardFound) {
    if (clip.projection != camera::Projection::DualFisheye || clip.lenses.size() != 2) {
        err = "not a dual-fisheye clip";
        return false;
    }
    if (!clip.calibrated) {
        err = "no extended (MEI) lens calibration in the file";
        return false;
    }
    calib.lensesExt = {clip.lenses[0].calibration, clip.lenses[1].calibration};
    params.outYawDeg = clip.mountYawDeg;
    params.outPitchDeg = clip.mountPitchDeg;
    params.outRollDeg = clip.mountRollDeg;
    const insv::LensProfile* bare = clip.bareLens ? &*clip.bareLens : nullptr;
    if (calibration && !calibration->matchesFile(calib.lensesExt[0], calib.lensesExt[1])) {
        std::fprintf(stderr, "[stitch] the lens calibration of %s isn't for this file's "
                             "camera calibration; stitching with the file's\n",
                     calibration->created.c_str());
        calibration = nullptr;
    }
    if (calibration) {
        // The guard was over the lenses when the chart was filmed: it is in
        // the calibration, so the lens is taken as it is.
        ApplyLensCalibration(*calibration, calib.lensesExt[0], calib.lensesExt[1]);
        insv::ApplyLensProfile(params, bare, nullptr);
        if (guardFound)
            *guardFound = true;
        return true;
    }
    if (modelCorrection)
        params.modelCorrection = clip.lensCorrection;
    const insv::LensProfile* acc = LensGuardProfile(clip, guard);
    bool found = insv::ApplyLensProfile(params, bare, acc);
    if (guard != PreviewStitchConfig::LensGuard::None && !acc)
        found = false;
    if (guardFound)
        *guardFound = found;
    return true;
}

NativeStitchSource::NativeStitchSource() = default;
NativeStitchSource::~NativeStitchSource() {
    // Held / kept frames go before the decoder.
    while (!kept_.empty())
        ReleaseKeptPair(kept_.begin()->first);
    // Tear the GPU stitch down before the decoder: on the all-GPU path the
    // stitcher borrows the decoder's VkDevice.
    gpu_.reset();
    vdec_.reset();
}

bool NativeStitchSource::Open(const std::string& clip_path, const PreviewStitchConfig& config) {
    while (!kept_.empty())
        ReleaseKeptPair(kept_.begin()->first);
    vdec_.reset();
    demux_.reset();
    stitcher_.reset();
    gpu_.reset();
    info_ = ClipInfo{};
    last_error_.clear();
    gpu_fallback_ = {};
    gpu_released_ = false;

    std::string err;
    const auto clip = camera::ReadClipMetadata(clip_path, &err);
    if (!clip) {
        last_error_ = err + ": " + clip_path;
        return false;
    }
    insv::InsvCalibration calib;
    insv::StitchParams params;
    bool guardFound = true;
    const LensCalibration* lensCal = config.lensCalibration ? &*config.lensCalibration : nullptr;
    if (!DualFisheyeStitchSetup(*clip, config.lensGuard, lensCal,
                                config.method == PreviewStitchConfig::Method::New, calib, params,
                                err, &guardFound)) {
        last_error_ = err + ": " + clip_path;
        return false;
    }
    if (!guardFound)
        std::fprintf(stderr, "[NativeStitchSource] no lens-guard profile in the file; "
                             "stitching as a bare lens\n");
    info_.width = clip->lensWidth;
    info_.height = clip->lensHeight;
    info_.fps = clip->fps;
    info_.duration_ms = clip->durationMs;

    eq_w_ = config.width > 0 ? config.width : AutoPreviewWidth(info_.width, info_.fps);
    eq_h_ = config.width > 0 && config.height > 0 ? config.height : eq_w_ / 2;

    params.depthSeam = config.depthSeam();
    method_ = config.method;
    seam_colour_on_ = config.seamColourBlendOn();
    seam_colour_window_.reset();
    clip_path_ = clip_path;
    lens_guard_ = config.lensGuard;
    lens_calibration_ = config.lensCalibration;
    analysis_dirty_ = true;
    seam_state_ = insv::SeamState{};
    if (!seam_costs_)
        seam_costs_ = std::make_unique<insv::SeamCosts>();
    if (!cut_stats_)
        cut_stats_ = std::make_unique<insv::SeamCutStats>();
    auto stitcher = std::make_unique<insv::FisheyeStitcher>();
    if (!stitcher->configure(calib, info_.width, info_.height, params)) {
        last_error_ = "FisheyeStitcher::configure failed: " + clip_path;
        return false;
    }
    gpu_params_ = stitcher->gpuParams();
    // The seam flow: its grid over the overlap both lenses see, carried in
    // every frame's params for the GPU's strip pass. On the GPU stitch when
    // there is one (settleSeamFlow, once that's known), else with OpenCV.
    seam_flow_on_ = false;
    seam_flow_grid_ = {};
    seam_flow_gpu_ = false;
    seam_flow_window_.reset();
    seam_fade_window_.reset();
    seam_flow_field_.reset();
    if (config.seamFlowOn()) {
        seam_flow_grid_ = insv::SeamFlowGridFor(*stitcher);
        if (!seam_flow_grid_.valid()) {
            core::log::Info("[NativeStitchSource] the lenses share too little round the seam for "
                            "the seam flow; stitching without it\n");
        } else {
            seam_flow_on_ = true;
            gpu_params_.seamFlowCols = seam_flow_grid_.cols;
            gpu_params_.seamFlowRows = seam_flow_grid_.rows;
            gpu_params_.seamFlowStripRad = seam_flow_grid_.stripRad;
            gpu_params_.seamFlowReachRad = seam_flow_grid_.reachRad;
            if (!seam_flow_strips_)
                seam_flow_strips_ = std::make_unique<insv::SeamFlowStrips>();
#ifdef ORBIS_SEAM_FLOW
            if (!seam_flow_estimator_)
                seam_flow_estimator_ = std::make_shared<insv::SeamFlowEstimator>();
#endif
        }
    }
    seam_window_.configure(stitcher->baselineM(), gpu_params_.bandRad);
    seam_window_.setLensColour(params.lensColour);
    calib_ = calib;
    params_ = params;
    stitcher_ = std::move(stitcher);
    seam_flow_wanted_ = seam_flow_on_;
    lens_sources_ = clip->lensSources();
    gpu_released_ = false;
    resume_seek_ = false;
    resume_at_ms_ = 0;
    return openDecode();
}

bool NativeStitchSource::openDecode() {
    gpu_fallback_ = {};
    // settleSeamFlow may have turned the seam flow off for want of a GPU
    // stitch last time; this time there may be one.
    if (seam_flow_wanted_ && !seam_flow_on_) {
        seam_flow_on_ = true;
        gpu_params_.seamFlowCols = seam_flow_grid_.cols;
        gpu_params_.seamFlowRows = seam_flow_grid_.rows;
    }

    // --- Stage 2: all-GPU (Vulkan video decode -> GPU stitch on one device) ---
    // Set when the GPU should have decoded and didn't (GetGpuFallback), or
    // decodes video but not this codec: a GPU without any video decode is
    // said once at startup instead, not per clip.
    bool decodeFailed = false;
    std::string decodeCodec;
    if (!std::getenv("INSV_NO_GPU_DECODE") && !std::getenv("INSV_NO_GPU_STITCH")) {
        auto vdec = std::make_unique<insv::VulkanVideoDecoder>();
        std::string derr;
        if (vdec->open(lens_sources_, derr)) {
            VkInstance inst = VK_NULL_HANDLE;
            VkPhysicalDevice phys = VK_NULL_HANDLE;
            VkDevice dev = VK_NULL_HANDLE;
            uint32_t qf = 0;
            VkDeviceQueueCreateFlags qflags = 0;
            vdec->deviceHandles(inst, phys, dev, qf, qflags);
            auto gpu = std::make_unique<render::VulkanFisheyeStitcher>();
            std::string gerr;
            // The stitcher's ycbcr conversion is built for one image format,
            // so ask the decoder what it actually produces rather than infer
            // it from the stream's bit depth. A format the stitcher can't
            // sample (or none) fails initAdopt: CPU decode below.
            const VkFormat lensFormat = vdec->frameFormat();
            if (gpu->initAdopt(inst, phys, dev, qf, qflags, gerr, lensFormat,
                               vdec->lens(0).ycbcr)) {
                auto ql = vdec->queueLock(qf, 0);
                gpu->setQueueLock(std::move(ql.lock), std::move(ql.unlock));
                gpu->setDeviceKeepAlive(vdec->deviceKeepAlive());
                vdec_ = std::move(vdec);
                gpu_ = std::move(gpu);
                core::log::Info("[NativeStitchSource] all-GPU path: Vulkan video decode + "
                                "compute stitch\n");
                settleSeamFlow();
                return true;
            }
            std::fprintf(stderr, "[NativeStitchSource] GPU stitch on the decoder's device "
                                 "failed (%s); falling back to CPU decode\n",
                         gerr.c_str());
            decodeFailed = true;
        } else {
            std::fprintf(stderr, "[NativeStitchSource] Vulkan video decode unavailable (%s); "
                                 "CPU decode\n",
                         derr.c_str());
            decodeCodec = vdec->unsupportedCodec();
            decodeFailed = !vdec->unsupported() || !decodeCodec.empty();
        }
    }

    // --- Stage 1 / fallback: CPU decode + (GPU or CPU) stitch ---
    auto demux = std::make_unique<insv::InsvDemux>();
    std::string err;
    if (!demux->open(lens_sources_, err)) {
        last_error_ = "demux open failed: " + err + ": " + clip_path_;
        return false;
    }
    if (!std::getenv("INSV_NO_GPU_STITCH")) {
        auto gpu = std::make_unique<render::VulkanFisheyeStitcher>();
        std::string gerr;
        if (gpu->init(gerr))
            gpu_ = std::move(gpu);
        else
            std::fprintf(stderr, "[NativeStitchSource] GPU stitch unavailable (%s); using CPU\n",
                         gerr.c_str());
    }
    gpu_fallback_.decode = decodeFailed;
    gpu_fallback_.codec = decodeCodec;
    gpu_fallback_.stitch = !gpu_ && !std::getenv("INSV_NO_GPU_STITCH");
    demux_ = std::move(demux);
    settleSeamFlow();
    return true;
}

void NativeStitchSource::settleSeamFlow() {
    if (!seam_flow_on_)
        return;
    if (gpu_) {
        gpu_->resetSeamFlow();
        return;
    }
    if (!seam_flow_estimator_) {
        core::log::Info("[NativeStitchSource] no seam flow without a GPU stitch in this build "
                        "(the CPU's needs OpenCV); stitching without it\n");
        seam_flow_on_ = false;
        gpu_params_.seamFlowCols = gpu_params_.seamFlowRows = 0;
    }
}

bool NativeStitchSource::SeekTo(int64_t timestamp_ms) {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    if (!ResumeGpu())
        return false;
    const int64_t t = timestamp_ms < 0 ? 0 : timestamp_ms;
    resume_seek_ = false;
    resume_at_ms_ = t;
    const bool quick = quick_seek_;
    quick_seek_ = false;
    scrub_frame_ = quick;
    seam_window_.reset(); // the frames measured so far are about another stretch
    if (!quick)           // a scrub's frame keeps the last frame's seam
        seam_state_ = insv::SeamState{};
    seam_colour_window_.reset();
    seam_flow_window_.reset();
    seam_fade_window_.reset();
    if (gpu_)
        gpu_->resetSeamFlow();
    if (!quick) {
        seam_flow_field_.reset();
        seam_flow_gpu_ = false;
    }
    if ((seam_colour_on_ || seam_flow_on_) && !quick && t > 0 && !preRollSeam(t))
        return false;
    return seekDecoder(t);
}

bool NativeStitchSource::preRollSeam(int64_t t) {
    // The seam colour blend pools the quarter second before a frame, the seam flow
    // the few frames before it: measure them now, so the frame at t is
    // stitched as playback into it has it. Every frame before the one the
    // seek below lands on (the first at or after t) counts, so read on
    // until a frame at or after t; that one is read again after the seek.
    const double frameMs = info_.fps > 0 ? 1000.0 / info_.fps : 33.4;
    const int64_t flowFrom =
        seam_flow_on_ ? t - std::llround((insv::kSeamFlowFrames - 0.5) * frameMs) : INT64_MAX;
    int64_t from = t;
    if (seam_colour_on_)
        from = t - seam_colour_window_.lookBackMs();
    from = std::min(from, flowFrom);
    if (!seekDecoder(std::max<int64_t>(0, from)))
        return false;
    for (;;) {
        int64_t pts = 0;
        bool flowMeasured = false;
        if (!measureNextSeamFrame(pts, flowFrom, t, &flowMeasured))
            break; // the end, or an error: the frames so far
        if (pts >= t)
            break; // the seek's own frame: measured when it's read
        if (seam_colour_on_)
            seam_colour_window_.add(pts, *cut_stats_, gpu_params_.bandRad);
        if (flowMeasured)
            addSeamFlow();
    }
    return true;
}

bool NativeStitchSource::addSeamFlow() {
    if (!seam_flow_estimator_ || !seam_flow_strips_)
        return false;
    insv::SeamFlowField f;
    if (!seam_flow_estimator_->estimate(*seam_flow_strips_, f))
        return false;
    if (seam_colour_on_) {
        insv::SeamFadeFrame fade;
        insv::MeasureSeamFadeFrame(*seam_flow_strips_, f, fade);
        seam_fade_window_.add(std::move(fade));
    }
    seam_flow_window_.add(std::move(f));
    auto solved = std::make_shared<insv::SeamFlowField>();
    seam_flow_window_.solve(*solved);
    seam_flow_field_ = std::move(solved);
    return true;
}

bool NativeStitchSource::seekDecoder(int64_t t) {
    std::string err;
    if (vdec_) {
        if (!vdec_->seekTo(t, err)) {
            last_error_ = "seek failed: " + err;
            return false;
        }
        return true;
    }
    if (!demux_) {
        last_error_ = "SeekTo before a successful Open";
        return false;
    }
    if (!demux_->seekTo(t, err)) {
        last_error_ = "seek failed: " + err;
        return false;
    }
    return true;
}

void NativeStitchSource::copyGpuTiming() {
    const render::StitchTiming& t = gpu_->lastTiming();
    last_timing_.seamMs = seam_ms_;
    last_timing_.fenceWaitMs = t.fenceWaitMs;
    last_timing_.stitchGpuMs = t.stitchGpuMs;
    last_timing_.reframeGpuMs = t.reframeGpuMs;
    last_timing_.copyMs = t.copyMs;
}

int64_t NativeStitchSource::NearestKeyframeMs(int64_t timestamp_ms) const {
    if (vdec_)
        return vdec_->nearestKeyframeMs(timestamp_ms);
    if (demux_)
        return demux_->nearestKeyframeMs(timestamp_ms);
    return timestamp_ms;
}

void NativeStitchSource::KeyframesAround(int64_t timestamp_ms, int64_t& before, int64_t& after) const {
    before = after = -1;
    if (vdec_)
        vdec_->keyframesAroundMs(timestamp_ms, before, after);
    else if (demux_)
        demux_->keyframesAroundMs(timestamp_ms, before, after);
}

Frame NativeStitchSource::readGpuAllVulkan() {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    insv::FramePts pts;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = vdec_->readNextPair(f0, f1, &pts, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return Frame{};
    }
    resume_at_ms_ = pts.ms + 1; // a resume carries on after it

    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(f0, lf0);
    vdec_->lockFrame(f1, lf1);

    Frame frame;
    frame.width = eq_w_;
    frame.height = eq_h_;
    frame.timestamp_ms = pts.ms;
    frame.timestamp_us = pts.us;
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    const insv::StitchGpuParams params = paramsWithSeam(pts, in0, in1);
    const bool ok = gpu_->stitchVk(in0, in1, params, eq_w_, eq_h_, frame.rgba, &oc0, &oc1, gerr);

    insv::VulkanLensFrameResult r0{oc0.semSignalValue, oc0.newLayout};
    insv::VulkanLensFrameResult r1{oc1.semSignalValue, oc1.newLayout};
    if (ok) {
        vdec_->unlockFrame(f0, r0);
        vdec_->unlockFrame(f1, r1);
    } else {
        // On failure the frames are as the seam measure (if any) left them.
        vdec_->unlockFrame(f0, {in0.semWaitValue, in0.layout});
        vdec_->unlockFrame(f1, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);

    if (!ok) {
        last_error_ = "GPU stitchVk failed: " + gerr;
        return Frame{};
    }
    copyGpuTiming();
    return frame;
}

void NativeStitchSource::SetSeamCorrection(const insv::SeamCorrection& c) {
    if (!stitcher_ || c == params_.seam || method_ == PreviewStitchConfig::Method::New)
        return;
    params_.seam = c;
    if (stitcher_->configure(calib_, info_.width, info_.height, params_)) {
        gpu_params_ = stitcher_->gpuParams(); // keeps the per-frame RS fields: set in paramsAt
        seam_window_.configure(stitcher_->baselineM(), gpu_params_.bandRad); // frames measured on the old one go
    }
    analysis_dirty_ = true;
}

void NativeStitchSource::SetLensColour(const insv::LensColour& c) {
    if (!stitcher_ || c == params_.lensColour || method_ == PreviewStitchConfig::Method::New)
        return;
    params_.lensColour = c;
    seam_window_.setLensColour(c); // the frames' residuals are measured against it
    analysis_dirty_ = true;
    if (stitcher_->configure(calib_, info_.width, info_.height, params_))
        gpu_params_ = stitcher_->gpuParams();
}

void NativeStitchSource::SetCaptureTiming(CaptureTiming timing, int64_t untilMs,
                                          CaptureTiming after) {
    std::lock_guard<std::mutex> lk(rs_mx_);
    capture_timing_ = std::move(timing);
    capture_until_ms_ = untilMs;
    capture_after_ = std::move(after);
}

bool NativeStitchSource::SetBlurQuads(BlurQuadsFn fn) {
    std::lock_guard<std::mutex> lk(rs_mx_);
    blur_quads_ = std::move(fn);
    return true;
}

void NativeStitchSource::SetSeamAnalysisEnabled(bool on) {
    analysis_enabled_ = on;
    analysis_dirty_ = true;
}

void NativeStitchSource::ConfigureSeamWindow(insv::SeamWindow& w) const {
    if (!stitcher_)
        return;
    w.configure(stitcher_->baselineM(), gpu_params_.bandRad);
    w.setLensColour(params_.lensColour);
}

bool NativeStitchSource::MeasureNextSeamFrame(insv::SeamWindow& w, int64_t& pts_ms) {
    last_error_.clear();
    if (!resumeForRead())
        return false;
    float q[insv::kSeamDepthBins];
    insv::FramePts pts;
    if (vdec_) {
        ReleaseHeldPair();
        AVFrame* f0 = nullptr;
        AVFrame* f1 = nullptr;
        std::string err;
        if (!vdec_->readNextPair(f0, f1, &pts, err)) {
            last_error_ = err; // empty at EOS
            return false;
        }
        resume_at_ms_ = pts.ms + 1; // a resume carries on after it
        insv::VulkanLensFrame lf0, lf1;
        vdec_->lockFrame(f0, lf0);
        vdec_->lockFrame(f1, lf1);
        render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
        insv::StitchGpuParams p = gpu_params_;
        pts_ms = pts.ms;
        w.measureTable(pts_ms, q);
        std::copy(q, q + insv::kSeamDepthBins, p.seamQ);
        render::VulkanLensOutcome o0, o1;
        const bool ok = gpu_->measureSeamVk(in0, in1, p, *seam_costs_, cut_stats_.get(), &o0, &o1, err);
        if (ok) {
            vdec_->unlockFrame(f0, {o0.semSignalValue, o0.newLayout});
            vdec_->unlockFrame(f1, {o1.semSignalValue, o1.newLayout});
        } else {
            vdec_->unlockFrame(f0, {in0.semWaitValue, in0.layout});
            vdec_->unlockFrame(f1, {in1.semWaitValue, in1.layout});
        }
        vdec_->freeFrame(f0);
        vdec_->freeFrame(f1);
        if (!ok) {
            last_error_ = "seam measure failed: " + err;
            return false;
        }
    } else if (demux_ && stitcher_) {
        insv::Image a, b;
        std::string err;
        if (!demux_->readNextPair(a, b, &pts, err)) {
            last_error_ = err;
            return false;
        }
        resume_at_ms_ = pts.ms + 1; // a resume carries on after it
        pts_ms = pts.ms;
        w.measureTable(pts_ms, q);
        insv::MeasureSeamCutStats(*stitcher_, a, b, q, *cut_stats_);
        insv::MeasureSeamCosts(*stitcher_, a, b, *seam_costs_);
    } else {
        last_error_ = "MeasureNextSeamFrame before a successful Open";
        return false;
    }
    w.add(pts_ms, *seam_costs_, *cut_stats_);
    return true;
}

insv::StitchGpuParams NativeStitchSource::paramsAt(const insv::FramePts& pts) const {
    insv::StitchGpuParams p = gpu_params_;
    CaptureTiming ct;
    BlurQuadsFn blur;
    {
        std::lock_guard<std::mutex> lk(rs_mx_);
        // copy: call rotationAt outside the lock
        ct = pts.ms < capture_until_ms_ ? capture_timing_ : capture_after_;
        blur = blur_quads_;
    }
    p.blurCount = 0;
    if (blur) {
        std::vector<insv::BlurQuad> quads;
        blur(pts.ms, quads);
        for (const insv::BlurQuad& q : quads) {
            if (p.blurCount == insv::kMaxBlurQuads) {
                if (!blur_warned_) {
                    blur_warned_ = true;
                    std::fprintf(stderr, "[NativeStitchSource] %zu blur regions at %lld ms; only %d "
                                         "fit\n",
                                 quads.size(), static_cast<long long>(pts.ms), insv::kMaxBlurQuads);
                }
                break;
            }
            if (insv::prepareBlurQuad(q, p.blur[p.blurCount]))
                ++p.blurCount;
        }
    }
    if (!ct.active())
        return p;
    const double t = pts.sec();
    constexpr int kTaps = insv::StitchGpuParams::kRsTaps;
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < kTaps; ++k)
            ct.rotationAt(t,
                          ct.lensOffsetSec[i] +
                              (static_cast<double>(k) / (kTaps - 1) - 0.5) * ct.readoutSec[i],
                          p.rsRot[i][k]);
    p.rsActive = 1;
    p.rsUseColumns = ct.scanColumns ? 1 : 0;
    return p;
}

bool NativeStitchSource::seamStateFor(int64_t pts_ms,
                                      const std::function<bool(const float* q)>& measure) {
    if (analysis_dirty_) {
        analysis_dirty_ = false;
        analysis_.reset();
        if (analysis_enabled_ && params_.depthSeam && !clip_path_.empty())
            analysis_ = SeamAnalysisFor(
                {clip_path_, lens_guard_, params_.seam, params_.lensColour, lens_calibration_});
    }
    if (analysis_ && analysis_->stateAt(pts_ms, seam_state_))
        return true; // as every path stitches this frame
    float q[insv::kSeamDepthBins];
    seam_window_.measureTable(pts_ms, q);
    if (!measure(q))
        return false;
    seam_window_.add(pts_ms, *seam_costs_, *cut_stats_);
    seam_window_.solve(pts_ms, seam_state_);
    seam_window_.dropBefore(pts_ms - seam_window_.lookBackMs());
    return true;
}

insv::StitchGpuParams NativeStitchSource::paramsWithSeam(const insv::FramePts& pts,
                                                         render::VulkanLensInput& in0,
                                                         render::VulkanLensInput& in1) {
    insv::StitchGpuParams p = paramsAt(pts);
    seam_ms_ = 0.0;
    const bool scrub = scrub_frame_;
    scrub_frame_ = false;
    if (scrub) {
        // A scrub's passing frame: no seam measurement, the last frame's
        // seam (the depth seam's from its analysis where it has the frame).
        if (seam_colour_on_)
            heldSeamColour(p);
        if (seam_flow_on_)
            p.seamFlowGpu = seam_flow_gpu_;
        if (p.depthActive) {
            seamStateFor(pts.ms, [](const float*) { return false; });
            std::copy(seam_state_.q, seam_state_.q + insv::kSeamDepthBins, p.seamQ);
            p.seamBlend = seam_state_.blend;
        }
        return p;
    }
    if (seam_colour_on_ || seam_flow_on_) {
        // The new stitcher's layers measured in the overlap at the
        // template's geometry (no depth table: it has none) -- the lenses'
        // colour, the seam flow's strips -- in one submission of their own,
        // so this frame's stitch uses them.
        const auto t0 = std::chrono::steady_clock::now();
        render::VulkanLensOutcome o0, o1;
        std::string err;
        const bool measured =
            seam_flow_on_
                ? gpu_->measureSeamFlowVk(in0, in1, p, seam_colour_on_ ? cut_stats_.get() : nullptr,
                                          &o0, &o1, err)
                : gpu_->measureSeamCutVk(in0, in1, p, *cut_stats_, &o0, &o1, err);
        if (measured) {
            in0.semWaitValue = o0.semSignalValue;
            in0.layout = o0.newLayout;
            in1.semWaitValue = o1.semSignalValue;
            in1.layout = o1.newLayout;
        } else {
            if (!seam_warned_) {
                seam_warned_ = true;
                std::fprintf(stderr, "[NativeStitchSource] seam %s measure failed (%s); "
                                     "stitching without it\n",
                             seam_flow_on_ ? "flow" : "colour", err.c_str());
            }
            // Without the flow from the next frame on, so the colour blend
            // still measures.
            seam_flow_on_ = false;
            seam_flow_gpu_ = false;
        }
        if (seam_colour_on_)
            applySeamColour(pts.ms, p, [&] { return measured; });
        if (seam_flow_on_) {
            if (measured)
                seam_flow_gpu_ = true;
            p.seamFlowGpu = seam_flow_gpu_;
        }
        seam_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                       .count();
    }
    if (!p.depthActive)
        return p;
    const auto t0 = std::chrono::steady_clock::now();
    seamStateFor(pts.ms, [&](const float* q) {
        // The cut is measured on the table of the frames before, in the same
        // submission as the cost volume.
        insv::StitchGpuParams m = p;
        std::copy(q, q + insv::kSeamDepthBins, m.seamQ);
        render::VulkanLensOutcome o0, o1;
        std::string err;
        if (!gpu_->measureSeamVk(in0, in1, m, *seam_costs_, cut_stats_.get(), &o0, &o1, err)) {
            if (!seam_warned_) {
                seam_warned_ = true;
                std::fprintf(stderr, "[NativeStitchSource] seam depth measure failed (%s); "
                                     "keeping the last table\n",
                             err.c_str());
            }
            return false;
        }
        in0.semWaitValue = o0.semSignalValue;
        in0.layout = o0.newLayout;
        in1.semWaitValue = o1.semSignalValue;
        in1.layout = o1.newLayout;
        return true;
    });
    std::copy(seam_state_.q, seam_state_.q + insv::kSeamDepthBins, p.seamQ);
    p.seamBlend = seam_state_.blend;
    seam_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                   .count();
    return p;
}

void NativeStitchSource::heldSeamColour(insv::StitchGpuParams& p) const {
    p.seamColourActive = 1;
    std::copy(&seam_colour_table_[0][0], &seam_colour_table_[0][0] + insv::kSeamDepthBins * 3,
              &p.seamColour[0][0]);
    std::copy(&seam_veil_[0][0], &seam_veil_[0][0] + insv::kSeamDepthBins * 3, &p.seamVeil[0][0]);
}

bool NativeStitchSource::applySeamColour(int64_t pts_ms, insv::StitchGpuParams& p,
                                         const std::function<bool()>& measure) {
    if (!measure())
        return false;
    seam_colour_window_.add(pts_ms, *cut_stats_, gpu_params_.bandRad);
    seam_colour_window_.solve(pts_ms, &seam_colour_table_[0][0], &seam_veil_[0][0]);
    seam_colour_window_.dropBefore(pts_ms - seam_colour_window_.lookBackMs());
    heldSeamColour(p);
    return true;
}

bool NativeStitchSource::measureNextSeamFrame(int64_t& pts_ms, int64_t flowFrom, int64_t until,
                                              bool* flowMeasured) {
    last_error_.clear();
    *flowMeasured = false;
    insv::FramePts pts;
    if (vdec_) {
        ReleaseHeldPair();
        AVFrame* f0 = nullptr;
        AVFrame* f1 = nullptr;
        std::string err;
        if (!vdec_->readNextPair(f0, f1, &pts, err)) {
            last_error_ = err; // empty at EOS
            return false;
        }
        pts_ms = pts.ms;
        insv::VulkanLensFrame lf0, lf1;
        vdec_->lockFrame(f0, lf0);
        vdec_->lockFrame(f1, lf1);
        render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
        render::VulkanLensOutcome o0, o1;
        // Measured as playback measures this frame: with its capture-time
        // correction, which moves what the strips see.
        const insv::StitchGpuParams p = paramsAt(pts);
        const bool measure = pts_ms < until;
        const bool flow = measure && seam_flow_on_ && pts_ms >= flowFrom;
        bool ok = true;
        if (flow)
            ok = gpu_->measureSeamFlowVk(in0, in1, p, seam_colour_on_ ? cut_stats_.get() : nullptr,
                                         &o0, &o1, err);
        else if (measure && seam_colour_on_)
            ok = gpu_->measureSeamCutVk(in0, in1, p, *cut_stats_, &o0, &o1, err);
        else
            o0 = {in0.semWaitValue, in0.layout}, o1 = {in1.semWaitValue, in1.layout};
        if (flow && ok)
            seam_flow_gpu_ = true;
        if (ok) {
            vdec_->unlockFrame(f0, {o0.semSignalValue, o0.newLayout});
            vdec_->unlockFrame(f1, {o1.semSignalValue, o1.newLayout});
        } else {
            vdec_->unlockFrame(f0, {in0.semWaitValue, in0.layout});
            vdec_->unlockFrame(f1, {in1.semWaitValue, in1.layout});
        }
        vdec_->freeFrame(f0);
        vdec_->freeFrame(f1);
        if (!ok) {
            last_error_ = "seam measure failed: " + err;
            return false;
        }
    } else if (demux_ && stitcher_) {
        insv::Image a, b;
        std::string err;
        if (!demux_->readNextPair(a, b, &pts, err)) {
            last_error_ = err;
            return false;
        }
        pts_ms = pts.ms;
        if (pts_ms >= until)
            return true;
        const float none[insv::kSeamDepthBins] = {};
        if (seam_colour_on_)
            insv::MeasureSeamCutStats(*stitcher_, a, b, none, *cut_stats_);
        if (seam_flow_on_ && pts_ms >= flowFrom) {
            if (gpu_) {
                // Measured as playback measures this frame (with its
                // capture-time correction).
                if (!gpu_->measureSeamFlow(a.rgb.data(), b.rgb.data(), paramsAt(pts), err)) {
                    last_error_ = "seam measure failed: " + err;
                    return false;
                }
                seam_flow_gpu_ = true;
            } else {
                insv::MeasureSeamFlowStrips(*stitcher_, seam_flow_grid_, a, b, *seam_flow_strips_);
                *flowMeasured = true;
            }
        }
    } else {
        last_error_ = "seam pre-roll before a successful Open";
        return false;
    }
    return true;
}

bool NativeStitchSource::SensorRowOf(const double eqDir[3], int& lens, double& rowFrac) const {
    if (!stitcher_ || info_.height <= 0)
        return false;
    const insv::Mat3& R = stitcher_->outRotation(); // stitch world = outRot * eqDir
    const insv::Vec3 w{R.m[0][0] * eqDir[0] + R.m[0][1] * eqDir[1] + R.m[0][2] * eqDir[2],
                       R.m[1][0] * eqDir[0] + R.m[1][1] * eqDir[1] + R.m[1][2] * eqDir[2],
                       R.m[2][0] * eqDir[0] + R.m[2][1] * eqDir[1] + R.m[2][2] * eqDir[2]};
    double u[2], v[2], th[2];
    bool ok[2];
    for (int i = 0; i < 2; ++i)
        ok[i] = stitcher_->project(i, w, u[i], v[i], th[i]);
    constexpr double kSeamBandRad = 12.0 * std::numbers::pi / 180.0;
    if (ok[0] && ok[1] && std::fabs(th[0] - th[1]) < kSeamBandRad)
        return false; // blended from both lenses
    if (!ok[0] && !ok[1])
        return false;
    lens = (ok[0] && (!ok[1] || th[0] < th[1])) ? 0 : 1;
    rowFrac = v[lens] / static_cast<double>(info_.height);
    return rowFrac >= 0.0 && rowFrac <= 1.0;
}

bool NativeStitchSource::GpuReframeAvailable() const {
    return vdec_ && gpu_ && (gpu_->reframeReady() || gpu_->viewReady());
}

bool NativeStitchSource::GpuViewAvailable() const {
    return vdec_ && gpu_ && gpu_->viewReady();
}

FlatFrame NativeStitchSource::ReadFlatFrame(int outW, int outH, const view::Pose* pose,
                                            const float stab[9],
                                            const color::Grade* colorGrade) {
    return ReadFlatFrame(
        outW, outH,
        [pose, stab](const insv::FramePts&, view::Pose& p, float s[9]) {
            if (pose)
                p = *pose;
            if (stab)
                std::copy(stab, stab + 9, s);
        },
        colorGrade);
}

FlatFrame NativeStitchSource::ReadFlatFrame(int outW, int outH, const FramePoseFn& perFrame,
                                            const color::Grade* colorGrade) {
    if (!resumeForRead())
        return FlatFrame{};
    if (!GpuReframeAvailable()) {
        last_error_ = "ReadFlatFrame without the all-GPU reproject path";
        return FlatFrame{};
    }
    // The fused pass unless it's missing or INSV_FLAT_VIA_EQUIRECT asks for
    // the old equirect + reproject (comparisons, and a way back if needed).
    const bool fused = GpuViewAvailable() &&
                       (!gpu_->reframeReady() || !std::getenv("INSV_FLAT_VIA_EQUIRECT"));
    FlatFrame frame;
    insv::FramePts pts;
    frame.p010 = fused && flat_ten_bit_;
    if (!readView(outW, outH, perFrame, colorGrade,
                  !fused         ? FlatPath::ViaEquirect
                  : frame.p010   ? FlatPath::FusedP010
                                 : FlatPath::FusedNv12,
                  frame.data, pts))
        return FlatFrame{};
    frame.timestamp_ms = pts.ms;
    frame.width = outW;
    frame.height = outH;
    return frame;
}

Frame NativeStitchSource::ReadViewFrame(int outW, int outH, const FramePoseFn& perFrame,
                                        const color::Grade* colorGrade) {
    if (!resumeForRead())
        return Frame{};
    if (!GpuViewAvailable()) {
        last_error_ = "ReadViewFrame without the fused GPU view path";
        return Frame{};
    }
    Frame frame;
    insv::FramePts pts;
    if (!readView(outW, outH, perFrame, colorGrade, FlatPath::FusedRgba, frame.rgba, pts))
        return Frame{};
    frame.timestamp_ms = pts.ms;
    frame.timestamp_us = pts.us;
    frame.width = outW;
    frame.height = outH;
    return frame;
}

void NativeStitchSource::SetViewPanes(std::vector<ViewPane> panes, PanePoseFn poseAt,
                                      uint32_t fillRgba) {
    panes_ = panes.size() > 1 ? std::move(panes) : std::vector<ViewPane>{};
    pane_pose_ = std::move(poseAt);
    pane_fill_ = fillRgba;
}

bool NativeStitchSource::readView(int outW, int outH, const FramePoseFn& perFrame,
                                  const color::Grade* colorGrade, FlatPath path,
                                  std::vector<uint8_t>& out, insv::FramePts& pts) {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    pts = {};
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = vdec_->readNextPair(f0, f1, &pts, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return false;
    }
    resume_at_ms_ = pts.ms + 1; // a resume carries on after it

    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(f0, lf0);
    vdec_->lockFrame(f1, lf1);

    view::Pose pose; // forward view, the reproject's default
    float stab[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (perFrame)
        perFrame(pts, pose, stab);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    const insv::StitchGpuParams params = paramsWithSeam(pts, in0, in1);
    const render::VulkanFisheyeStitcher::ViewFormat fmt =
        path == FlatPath::FusedRgba   ? render::VulkanFisheyeStitcher::ViewFormat::Rgba
        : path == FlatPath::FusedP010 ? render::VulkanFisheyeStitcher::ViewFormat::P010
                                      : render::VulkanFisheyeStitcher::ViewFormat::Nv12;
    // Split screen: every pane's view of this frame into the one frame.
    std::vector<render::VulkanFisheyeStitcher::ViewRect> views(panes_.size());
    for (size_t i = 0; i < panes_.size(); ++i) {
        render::VulkanFisheyeStitcher::ViewRect& v = views[i];
        if (i == 0)
            v.pose = pose;
        else if (pane_pose_)
            pane_pose_(pts.ms, static_cast<int>(i), v.pose);
        v.x = panes_[i].x;
        v.y = panes_[i].y;
        v.w = panes_[i].w;
        v.h = panes_[i].h;
    }
    const bool ok =
        !views.empty()
            ? (path == FlatPath::ViaEquirect
                   ? gpu_->stitchAndReframeViewsVk(in0, in1, params, eq_w_, eq_h_, outW, outH,
                                                   views, stab, colorGrade, pane_fill_, out, &oc0,
                                                   &oc1, gerr)
                   : gpu_->renderViewsVk(in0, in1, params, outW, outH, views, stab,
                                         colorGrade, fmt, pane_fill_, out, &oc0, &oc1, gerr))
        : path == FlatPath::ViaEquirect
            ? gpu_->stitchAndReframeVk(in0, in1, params, eq_w_, eq_h_, outW, outH, &pose, stab,
                                       colorGrade, out, &oc0, &oc1, gerr)
            : gpu_->renderViewVk(in0, in1, params, outW, outH, &pose, stab, colorGrade,
                                 fmt, out, &oc0, &oc1, gerr);

    insv::VulkanLensFrameResult r0{oc0.semSignalValue, oc0.newLayout};
    insv::VulkanLensFrameResult r1{oc1.semSignalValue, oc1.newLayout};
    if (ok) {
        vdec_->unlockFrame(f0, r0);
        vdec_->unlockFrame(f1, r1);
    } else {
        vdec_->unlockFrame(f0, {in0.semWaitValue, in0.layout});
        vdec_->unlockFrame(f1, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);

    if (!ok) {
        last_error_ = std::string(path == FlatPath::ViaEquirect ? "GPU stitchAndReframeVk"
                                                                : "GPU renderViewVk") +
                      " failed: " + gerr;
        return false;
    }
    copyGpuTiming();
    return true;
}

struct NativeStitchSource::HeldPair {
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    insv::StitchGpuParams params;
    insv::FramePts pts;
};

int NativeStitchSource::KeepNextPair(insv::FramePts& pts) {
    if (!resumeForRead())
        return 0;
    if (!GpuViewAvailable()) {
        last_error_ = "KeepNextPair without the fused view path";
        return 0;
    }
    auto h = std::make_unique<HeldPair>();
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = vdec_->readNextPair(h->f0, h->f1, &h->pts, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return 0;
    }
    resume_at_ms_ = h->pts.ms + 1; // a resume carries on after it
    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(h->f0, lf0);
    vdec_->lockFrame(h->f1, lf1);
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    h->params = paramsWithSeam(h->pts, in0, in1);
    vdec_->unlockFrame(h->f0, {in0.semWaitValue, in0.layout});
    vdec_->unlockFrame(h->f1, {in1.semWaitValue, in1.layout});
    pts = h->pts;
    const int id = next_kept_++;
    kept_.emplace(id, std::move(h));
    return id;
}

bool NativeStitchSource::RenderKeptViews(int handle, int canvasW, int canvasH,
                                         const std::vector<ViewRect>& views, const float stab[9],
                                         uint32_t clearRgba, std::vector<uint8_t>& rgba) {
    const auto it = kept_.find(handle);
    if (it == kept_.end()) {
        last_error_ = "RenderKeptViews without that kept pair";
        return false;
    }
    HeldPair& h = *it->second;
    std::vector<render::VulkanFisheyeStitcher::ViewRect> vr(views.size());
    for (size_t i = 0; i < views.size(); ++i) {
        vr[i].pose = views[i].pose;
        vr[i].x = views[i].x;
        vr[i].y = views[i].y;
        vr[i].w = views[i].w;
        vr[i].h = views[i].h;
    }
    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(h.f0, lf0);
    vdec_->lockFrame(h.f1, lf1);
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    const bool ok = gpu_->renderViewsVk(in0, in1, h.params, canvasW, canvasH, vr, stab,
                                        clearRgba, rgba, &oc0, &oc1, gerr);
    if (ok) {
        vdec_->unlockFrame(h.f0, {oc0.semSignalValue, oc0.newLayout});
        vdec_->unlockFrame(h.f1, {oc1.semSignalValue, oc1.newLayout});
        copyGpuTiming();
        return true;
    }
    vdec_->unlockFrame(h.f0, {in0.semWaitValue, in0.layout});
    vdec_->unlockFrame(h.f1, {in1.semWaitValue, in1.layout});
    last_error_ = "GPU renderViewsVk failed: " + gerr;
    return false;
}

void NativeStitchSource::ReleaseKeptPair(int handle) {
    const auto it = kept_.find(handle);
    if (it == kept_.end())
        return;
    vdec_->freeFrame(it->second->f0);
    vdec_->freeFrame(it->second->f1);
    kept_.erase(it);
    if (handle == held_)
        held_ = 0;
}

size_t NativeStitchSource::KeptPairBytes() const {
    // Two NV12 lens images (one decoded pair).
    size_t bytes = 0;
    if (vdec_)
        for (int i = 0; i < vdec_->lensCount(); ++i)
            bytes += size_t(vdec_->lens(i).width) * vdec_->lens(i).height * 3 / 2;
    return bytes;
}

bool NativeStitchSource::HoldNextPair(insv::FramePts& pts) {
    ReleaseHeldPair();
    held_ = KeepNextPair(pts);
    if (!held_ && last_error_ == "KeepNextPair without the fused view path")
        last_error_ = "HoldNextPair without the fused view path";
    return held_ != 0;
}

bool NativeStitchSource::RenderHeldViews(int canvasW, int canvasH, const std::vector<ViewRect>& views,
                                         const float stab[9], uint32_t clearRgba,
                                         std::vector<uint8_t>& rgba) {
    if (!held_) {
        last_error_ = "RenderHeldViews without a held pair";
        return false;
    }
    return RenderKeptViews(held_, canvasW, canvasH, views, stab, clearRgba, rgba);
}

void NativeStitchSource::ReleaseHeldPair() {
    if (held_)
        ReleaseKeptPair(held_);
}

bool NativeStitchSource::LensFramesAvailable() const {
    return vdec_ && gpu_ && gpu_->lensExportReady() && !std::getenv("INSV_PLAYBACK_EQUIRECT");
}

void NativeStitchSource::TrimLensFrames() {
    if (gpu_)
        gpu_->trimLensRing();
}

bool NativeStitchSource::ReleaseGpu() {
    // A clip that fell back to the CPU holds nothing there, but is let go
    // too: ResumeGpu tries the GPU again.
    if (gpu_released_ || !kept_.empty() || (!vdec_ && !gpu_ && !gpu_fallback_.any()))
        return false; // nothing there, or frames kept on it
    // The stitch first: on the all-GPU path it borrows the decoder's device.
    // Its lens ring lives on while a viewport shows one of its frames.
    gpu_.reset();
    vdec_.reset();
    demux_.reset(); // the CPU decode's: openDecode tries the GPU again
    gpu_released_ = true;
    resume_seek_ = true;
    return true;
}

bool NativeStitchSource::ResumeGpu() {
    if (!gpu_released_)
        return true;
    const auto t0 = std::chrono::steady_clock::now();
    if (!openDecode())
        return false; // still released: a later call tries again
    gpu_released_ = false;
    core::log::Info("[NativeStitchSource] back on the %s in %.0f ms: %s\n",
                    AllGpu() ? "GPU" : "CPU",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count(),
                    clip_path_.c_str());
    return true;
}

bool NativeStitchSource::resumeForRead() {
    if (!ResumeGpu())
        return false;
    if (!resume_seek_)
        return true;
    // Where the source was: the frame after the last one read. SeekTo clears
    // the flag; its pre-roll measures the seam into that frame as playing on
    // would have.
    return SeekTo(resume_at_ms_);
}

bool NativeStitchSource::ReadLensFrame(render::LensFrame& out) {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    if (!resumeForRead())
        return false;
    if (!LensFramesAvailable()) {
        last_error_ = "ReadLensFrame without the all-GPU lens export path";
        return false;
    }
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    insv::FramePts pts;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = vdec_->readNextPair(f0, f1, &pts, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return false;
    }
    resume_at_ms_ = pts.ms + 1; // a resume carries on after it

    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(f0, lf0);
    vdec_->lockFrame(f1, lf1);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    const insv::StitchGpuParams params = paramsWithSeam(pts, in0, in1);
    const bool ok = gpu_->fillLensFrameVk(in0, in1, params, out, &oc0, &oc1, gerr);
    if (ok) {
        vdec_->unlockFrame(f0, {oc0.semSignalValue, oc0.newLayout});
        vdec_->unlockFrame(f1, {oc1.semSignalValue, oc1.newLayout});
    } else {
        vdec_->unlockFrame(f0, {in0.semWaitValue, in0.layout});
        vdec_->unlockFrame(f1, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);
    if (!ok) {
        last_error_ = "GPU fillLensFrameVk failed: " + gerr;
        return false;
    }
    out.timestamp_ms = pts.ms;
    out.timestamp_us = pts.us;
    copyGpuTiming();
    return true;
}

Frame NativeStitchSource::readCpuDecode() {
    insv::Image a, b;
    insv::FramePts pts;
    std::string err;
    if (!demux_->readNextPair(a, b, &pts, err)) {
        last_error_ = err; // empty on a clean end-of-stream
        return Frame{};
    }
    resume_at_ms_ = pts.ms + 1; // a resume carries on after it

    // A scrub's passing frame is stitched with the last frame's seam, as the
    // GPU path.
    const bool scrub = scrub_frame_;
    scrub_frame_ = false;
    // Depth-aware seam: the cost volume on the CPU here (no GPU-resident
    // lenses to run the GPU pass on).
    if (params_.depthSeam) {
        const auto t0 = std::chrono::steady_clock::now();
        seamStateFor(pts.ms, [&](const float* q) { // as the GPU path
            if (scrub)
                return false;
            insv::MeasureSeamCutStats(*stitcher_, a, b, q, *cut_stats_);
            insv::MeasureSeamCosts(*stitcher_, a, b, *seam_costs_);
            return true;
        });
        last_timing_.seamMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
    }
    const float* seamQ = params_.depthSeam ? seam_state_.q : nullptr;
    const insv::SeamBlend* blend = params_.depthSeam ? &seam_state_.blend : nullptr;
    // The seam colour blend: the lenses' colour in the overlap at the
    // template's geometry, as the GPU path.
    insv::StitchGpuParams p = paramsAt(pts); // and the privacy blur quads
    if (seam_colour_on_ && scrub) {
        heldSeamColour(p);
    } else if (seam_colour_on_) {
        const auto t0 = std::chrono::steady_clock::now();
        applySeamColour(pts.ms, p, [&] {
            const float none[insv::kSeamDepthBins] = {};
            insv::MeasureSeamCutStats(*stitcher_, a, b, none, *cut_stats_);
            return true;
        });
        last_timing_.seamMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
    }
    // The seam flow: with the GPU stitch measured in its submission (below),
    // else its strips and flow on the CPU too.
    const bool flowOnGpu = seam_flow_on_ && gpu_;
    if (flowOnGpu) {
        p.seamFlowGpu = seam_flow_gpu_; // a scrub's frame: the last field
    } else if (seam_flow_on_) {
        if (!scrub) {
            const auto t0 = std::chrono::steady_clock::now();
            insv::MeasureSeamFlowStrips(*stitcher_, seam_flow_grid_, a, b, *seam_flow_strips_);
            addSeamFlow();
            // On top of the seam colour blend's time when it measured this frame.
            last_timing_.seamMs = (seam_colour_on_ ? last_timing_.seamMs : 0.0) +
                                  std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
        }
        p.seamFlow = seam_flow_field_;
        // With the colour blend, the fade between the lenses from its frames
        // and this frame's colour.
        if (seam_colour_on_ && seam_flow_field_ && !seam_flow_field_->empty() &&
            seam_fade_window_.size() > 0) {
            auto f = std::make_shared<insv::SeamFlowField>(*seam_flow_field_);
            seam_fade_window_.solve(&p.seamColour[0][0], f->fade);
            p.seamFlow = std::move(f);
        }
    }

    if (gpu_) {
        Frame f;
        f.width = eq_w_;
        f.height = eq_h_;
        f.timestamp_ms = pts.ms;
        f.timestamp_us = pts.us;
        std::string gerr;
        if (seamQ) {
            std::copy(seamQ, seamQ + insv::kSeamDepthBins, p.seamQ);
            p.seamBlend = *blend;
        }
        const bool measureFlow = flowOnGpu && !scrub;
        if (gpu_->stitch(a.rgb.data(), b.rgb.data(), p, eq_w_, eq_h_, f.rgba, gerr, measureFlow)) {
            if (measureFlow)
                seam_flow_gpu_ = true;
            return f;
        }
        std::fprintf(stderr, "[NativeStitchSource] GPU stitch failed (%s); falling back to CPU\n",
                     gerr.c_str());
        gpu_.reset();
        gpu_fallback_.stitch = true;
        // The CPU's seam flow from the next frame on, where there is one.
        if (seam_flow_on_ && !seam_flow_estimator_)
            seam_flow_on_ = false;
    }

    insv::Image eq;
    eq.alloc(eq_w_, eq_h_);
    stitcher_->renderEquirect(a, b, eq, seamQ, blend, p.blur, p.blurCount,
                              p.seamColourActive ? &p.seamColour[0][0] : nullptr, p.seamFlow.get(),
                              p.seamColourActive ? &p.seamVeil[0][0] : nullptr);
    return ToRgbaFrame(eq, pts);
}

Frame NativeStitchSource::ReadFrame() {
    if (!resumeForRead())
        return Frame{};
    if (vdec_)
        return readGpuAllVulkan();
    if (demux_ && stitcher_)
        return readCpuDecode();
    last_error_ = "ReadFrame before a successful Open";
    return Frame{};
}

Frame NativeStitchSource::GetFrame(int64_t timestamp_ms) {
    if (!SeekTo(timestamp_ms))
        return Frame{};
    return ReadFrame();
}

std::unique_ptr<IStitchSource> MakeStitchSource() {
    return std::make_unique<NativeStitchSource>();
}

} // namespace stitch
