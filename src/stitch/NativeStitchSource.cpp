#include "NativeStitchSource.h"

#include "../core/Log.h"
#include "../core/VulkanHwDevice.h" // core::sharedVulkanDevice
#include "ClipSeamAnalysis.h"

#include <orbis-core/stitch/LensModel.h>             // orbiscore::CameraCalibration
#include <orbis-core/stitch/FisheyeProjection.h>     // orbiscore::FisheyeStitcher, StitchParams
#include <orbis-core/Image.h>                 // orbiscore::Image
#include <orbis-core/media/FisheyeDemux.h>             // orbiscore::FisheyeDemux
#include <orbis-core/stitch/SeamFlow.h>              // SeamFlowStrips, MeasureSeamFlowStrips
#ifdef ORBIS_SEAM_FLOW
#include <orbis-core/stitch/SeamFlowEstimator.h>     // orbiscore::SeamFlowEstimator (OpenCV)
#endif
#include <VulkanFisheyeStitcher.h> // render::VulkanFisheyeStitcher

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}
#include <orbis-core/media/VulkanVideoDecoder.h>    // orbiscore::VulkanVideoDecoder (stage 2, GPU decode)

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

// RGB24 (orbiscore::Image) -> interleaved RGBA (stitch::Frame), opaque alpha.
Frame ToRgbaFrame(const orbiscore::Image& eq, const orbiscore::FramePts& pts) {
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

// `next` as the stitch's parameters, where the lenses are in the decoded
// frame kept from `p` (openDecode's, not the stitcher's).
void keepLensRect(orbiscore::StitchGpuParams& p, const orbiscore::StitchGpuParams& next) {
    float rect[2][4];
    std::memcpy(rect, p.lensRect, sizeof rect);
    p = next;
    std::memcpy(p.lensRect, rect, sizeof rect);
}

render::VulkanLensInput toRenderInput(const orbiscore::VulkanLensFrame& lf) {
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

const orbiscore::LensProfile* LensGuardProfile(const camera::ClipMetadata& clip,
                                               PreviewStitchConfig::LensGuard guard) {
    const std::optional<orbiscore::LensProfile>* p = nullptr;
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
                            orbiscore::CameraCalibration& calib, orbiscore::StitchParams& params,
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
    for (int i = 0; i < 2; ++i) {
        params.lensPose[i] = clip.lenses[i].pose;
        params.thetaK5[i] = clip.lenses[i].k5;
        params.lensMask[i] = clip.lenses[i].mask;
    }
    params.lensModel = clip.lenses[0].model;
    params.outYawDeg = clip.mountYawDeg;
    params.outPitchDeg = clip.mountPitchDeg;
    params.outRollDeg = clip.mountRollDeg;
    const orbiscore::LensProfile* bare = clip.bareLens ? &*clip.bareLens : nullptr;
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
        orbiscore::ApplyLensProfile(params, bare, nullptr);
        if (guardFound)
            *guardFound = true;
        return true;
    }
    if (modelCorrection)
        params.modelCorrection = clip.lensCorrection;
    const orbiscore::LensProfile* acc = LensGuardProfile(clip, guard);
    bool found = orbiscore::ApplyLensProfile(params, bare, acc);
    if (guard != PreviewStitchConfig::LensGuard::None && !acc)
        found = false;
    if (guardFound)
        *guardFound = found;
    return true;
}

NativeStitchSource::NativeStitchSource() = default;

// Proxy mode: the camera's preview file runs at its own rate (the X5's at
// 29.97 for a 120 fps clip, the Avata 360's at 25 for 100 fps), and a frame
// or two short. The clip keeps its own frames -- every frame time a full file
// has, so steps, trims, seeks and the timeline work as ever -- each showing
// the proxy frame on screen at that time: the last one at or before it
// (`held`), found by reading on while the next one (`ahead`) is due. Both
// files start at the same time (field 24 ties a proxy to its clip; every
// sample's first frame is at 0). Past the proxy's last frame it holds that.
struct NativeStitchSource::ProxyGrid {
    struct Span {
        int64_t startUs = 0;
        int64_t durationUs = 0;
    };
    std::vector<Span> parts; // the clip's, as its full files have them
    double fps = 0.0;        // the clip's frame rate
    double proxyFrameUs = 0.0;
    int64_t nextUs = 0;      // the clip frame the next read hands out; -1 past the end

    // The proxy frames: on the GPU decode path AVFrame pairs, else images.
    AVFrame* held[2] = {};
    AVFrame* ahead[2] = {};
    orbiscore::Image heldImg[2], aheadImg[2];
    bool heldValid = false, aheadValid = false, aheadEnd = false;
    orbiscore::FramePts heldPts, aheadPts;

    ~ProxyGrid() { drop(); }
    void drop()
    {
        for (int i = 0; i < 2; ++i) {
            av_frame_free(&held[i]);
            av_frame_free(&ahead[i]);
        }
        heldImg[0] = heldImg[1] = aheadImg[0] = aheadImg[1] = orbiscore::Image{};
        heldValid = aheadValid = aheadEnd = false;
    }
    // The clip's first frame at or after `us`; -1 past its last.
    int64_t frameAtOrAfter(int64_t us) const
    {
        for (const Span& p : parts) {
            if (us >= p.startUs + p.durationUs)
                continue;
            const double local = static_cast<double>(std::max<int64_t>(0, us - p.startUs));
            // A frame's own time, rounded to the microsecond or the ms, is
            // still that frame.
            const double j = std::ceil(local * fps * 1e-6 - 1e-3);
            const int64_t t = p.startUs + std::llround(j * 1e6 / fps);
            if (t < p.startUs + p.durationUs)
                return t;
        }
        return -1;
    }
    // Reads on until `held` is the proxy frame on screen at nextUs: `read`
    // fills `ahead` (false at the end, with `err` empty; on error, set).
    // False when there is none.
    template <class Read>
    bool settle(const Read& read, std::string& err)
    {
        // A proxy frame within half a ms of the clip frame is that frame
        // (the two round their times apart).
        constexpr int64_t kSlackUs = 500;
        for (;;) {
            if (!aheadValid && !aheadEnd) {
                if (read())
                    aheadValid = true;
                else if (!err.empty())
                    return false;
                else
                    aheadEnd = true;
            }
            if (aheadValid && (!heldValid || aheadPts.us <= nextUs + kSlackUs)) {
                for (int i = 0; i < 2; ++i) {
                    av_frame_free(&held[i]);
                    std::swap(held[i], ahead[i]);
                    std::swap(heldImg[i], aheadImg[i]);
                }
                heldPts = aheadPts;
                heldValid = true;
                aheadValid = false;
                continue;
            }
            return heldValid;
        }
    }
    // The handed-out pair's times, then on to the next clip frame. `repeat`:
    // the clip frame before it showed the same proxy frame -- told by the
    // times alone, so a seek and playing on into a frame agree.
    void advance(orbiscore::FramePts* pts, bool& repeat)
    {
        constexpr int64_t kSlackUs = 500; // as settle()'s
        if (pts) {
            pts->ms = std::llround(static_cast<double>(nextUs) * 1e-3);
            pts->us = heldPts.us;
        }
        const int64_t prevUs = nextUs - std::llround(1e6 / fps);
        repeat = false;
        for (const Span& p : parts)
            if (nextUs >= p.startUs && nextUs < p.startUs + p.durationUs)
                repeat = prevUs >= p.startUs - kSlackUs && heldPts.us <= prevUs + kSlackUs;
        nextUs = frameAtOrAfter(nextUs + std::llround(5e5 / fps));
    }
    // Where the decoder goes for a seek to clip time `ms`: a proxy frame
    // before the clip frame there, so settle() finds the one on screen.
    int64_t seekTo(int64_t ms)
    {
        drop();
        nextUs = frameAtOrAfter(ms * 1000);
        const int64_t at = nextUs < 0 ? ms * 1000 : nextUs;
        return std::max<int64_t>(0, static_cast<int64_t>(std::floor((at - proxyFrameUs) * 1e-3)));
    }
};

bool NativeStitchSource::proxyLensSize(const std::vector<orbiscore::ClipPart>& proxies, int& w,
                                       int& h, double& frameUs, std::string& err) {
    orbiscore::LensDemuxer demux;
    if (!demux.open(
            proxies, [](const auto*, const auto*, auto*, std::string&) { return true; }, err))
        return false;
    const orbiscore::InsvStreamInfo& s = demux.lens(0);
    const orbiscore::LensRect& r0 = proxies[0].lenses[0].rect;
    const orbiscore::LensRect& r1 = proxies[0].lenses[1].rect;
    w = static_cast<int>(std::lround(r0.w * s.width));
    h = static_cast<int>(std::lround(r0.h * s.height));
    // Square fisheyes, both alike: two halves of a 2:1 frame.
    if (w <= 0 || w != h || r1.w != r0.w || r1.h != r0.h || demux.lens(1).width != s.width) {
        err = "a " + std::to_string(s.width) + "x" + std::to_string(s.height) +
              " picture, not two lenses side by side";
        return false;
    }
    frameUs = s.fps > 0.0 ? 1e6 / s.fps : 1e6 / 30.0;
    return true;
}

bool NativeStitchSource::readPairVk(AVFrame*& f0, AVFrame*& f1, orbiscore::FramePts* pts,
                                    std::string& err) {
    picture_repeat_ = false;
    if (!proxy_)
        return vdec_->readNextPair(f0, f1, pts, err);
    ProxyGrid& g = *proxy_;
    err.clear();
    if (g.nextUs < 0)
        return false;
    const auto read = [&] { return vdec_->readNextPair(g.ahead[0], g.ahead[1], &g.aheadPts, err); };
    if (!g.settle(read, err))
        return false;
    f0 = av_frame_clone(g.held[0]);
    f1 = av_frame_clone(g.held[1]);
    if (!f0 || !f1) {
        av_frame_free(&f0);
        av_frame_free(&f1);
        err = "out of memory";
        return false;
    }
    g.advance(pts, picture_repeat_);
    return true;
}

bool NativeStitchSource::readPairCpu(orbiscore::Image& a, orbiscore::Image& b,
                                     orbiscore::FramePts* pts, std::string& err) {
    picture_repeat_ = false;
    if (!proxy_)
        return demux_->readNextPair(a, b, pts, err);
    ProxyGrid& g = *proxy_;
    err.clear();
    if (g.nextUs < 0)
        return false;
    const auto read = [&] {
        return demux_->readNextPair(g.aheadImg[0], g.aheadImg[1], &g.aheadPts, err);
    };
    if (!g.settle(read, err))
        return false;
    a = g.heldImg[0];
    b = g.heldImg[1];
    g.advance(pts, picture_repeat_);
    return true;
}
NativeStitchSource::~NativeStitchSource() {
    // Held / kept frames go before the decoder.
    while (!kept_.empty())
        ReleaseKeptPair(kept_.begin()->first);
    proxy_.reset();
    // Tear the GPU stitch down before the decoder: on the all-GPU path the
    // stitcher borrows the decoder's VkDevice.
    gpu_.reset();
    vdec_.reset();
}

bool NativeStitchSource::Open(const std::string& clip_path, const PreviewStitchConfig& config) {
    while (!kept_.empty())
        ReleaseKeptPair(kept_.begin()->first);
    proxy_.reset();
    vdec_.reset();
    demux_.reset();
    stitcher_.reset();
    gpu_.reset();
    info_ = ClipInfo{};
    last_error_.clear();
    gpu_fallback_ = {};
    gpu_released_ = false;
    has_proxy_ = false;

    std::string err;
    const auto clip = camera::ReadClipMetadata(clip_path, &err);
    if (!clip) {
        last_error_ = err + ": " + clip_path;
        return false;
    }
    orbiscore::CameraCalibration calib;
    orbiscore::StitchParams params;
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

    // The picture's files: the clip's own, or in proxy mode its preview
    // files, whose lenses are smaller (the calibration, stated at a reference
    // size, scales to them as to the full files).
    lens_sources_ = clip->partSources();
    has_proxy_ = !clip->proxySources().empty();
    int lensW = info_.width, lensH = info_.height;
    if (config.proxy) {
        auto grid = std::make_unique<ProxyGrid>();
        const std::vector<orbiscore::ClipPart> proxies = clip->proxySources();
        if (proxies.empty()) {
            core::log::Info("[NativeStitchSource] no preview file for every part of %s; playing "
                            "the full file\n", clip_path.c_str());
        } else if (proxyLensSize(proxies, lensW, lensH, grid->proxyFrameUs, err)) {
            for (const camera::Part& p : clip->parts)
                grid->parts.push_back({p.startUs, p.durationUs});
            grid->fps = info_.fps > 0.0 ? info_.fps : 30.0;
            lens_sources_ = proxies;
            proxy_ = std::move(grid);
            core::log::Info("[NativeStitchSource] playing from the preview file %s (%dx%d a "
                            "lens)\n", proxies[0].lenses[0].path.c_str(), lensW, lensH);
        } else {
            lensW = info_.width;
            lensH = info_.height;
            core::log::Info("[NativeStitchSource] the preview file of %s doesn't fit (%s); "
                            "playing the full file\n", clip_path.c_str(), err.c_str());
        }
    }

    eq_w_ = config.width > 0 ? config.width : AutoPreviewWidth(lensW, info_.fps);
    eq_h_ = config.width > 0 && config.height > 0 ? config.height : eq_w_ / 2;

    params.depthSeam = config.depthSeam();
    method_ = config.method;
    seam_colour_on_ = config.seamColourBlendOn();
    seam_colour_window_.reset();
    clip_path_ = clip_path;
    lens_guard_ = config.lensGuard;
    lens_calibration_ = config.lensCalibration;
    analysis_dirty_ = true;
    seam_state_ = orbiscore::SeamState{};
    if (!seam_costs_)
        seam_costs_ = std::make_unique<orbiscore::SeamCosts>();
    if (!cut_stats_)
        cut_stats_ = std::make_unique<orbiscore::SeamCutStats>();
    auto stitcher = std::make_unique<orbiscore::FisheyeStitcher>();
    lens_w_ = lensW;
    lens_h_ = lensH;
    if (!stitcher->configure(calib, lensW, lensH, params)) {
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
        seam_flow_grid_ = orbiscore::SeamFlowGridFor(*stitcher);
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
                seam_flow_strips_ = std::make_unique<orbiscore::SeamFlowStrips>();
#ifdef ORBIS_SEAM_FLOW
            if (!seam_flow_estimator_)
                seam_flow_estimator_ = std::make_shared<orbiscore::SeamFlowEstimator>();
#endif
        }
    }
    seam_window_.configure(stitcher->baselineM(), gpu_params_.bandRad);
    seam_window_.setLensColour(params.lensColour);
    calib_ = calib;
    params_ = params;
    stitcher_ = std::move(stitcher);
    seam_flow_wanted_ = seam_flow_on_;
    gpu_released_ = false;
    resume_seek_ = false;
    resume_at_ms_ = 0;
    return openDecode();
}

bool NativeStitchSource::openDecode() {
    gpu_fallback_ = {};
    // Where each lens is in the frame the stitch samples: the decoder's
    // whole frame, or its half of a proxy's (the CPU decode cuts the halves
    // out itself, so its lenses are whole frames).
    for (int i = 0; i < 2; ++i) {
        const orbiscore::LensRect whole;
        gpu_params_.lensRect[i][0] = static_cast<float>(whole.x);
        gpu_params_.lensRect[i][1] = static_cast<float>(whole.y);
        gpu_params_.lensRect[i][2] = static_cast<float>(whole.w);
        gpu_params_.lensRect[i][3] = static_cast<float>(whole.h);
    }
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
    if (!cpu_decode_ && !std::getenv("ORBIS_NO_GPU_DECODE") &&
        !std::getenv("ORBIS_NO_GPU_STITCH")) {
        auto vdec = std::make_unique<orbiscore::VulkanVideoDecoder>();
        std::string derr;
        // The process's one Vulkan device, which the stitch below adopts and
        // a Vulkan export encoder runs on too; the decoder keeps its own
        // reference.
        AVBufferRef* hw = core::sharedVulkanDevice();
        const bool opened = vdec->open(hw, lens_sources_, derr);
        av_buffer_unref(&hw);
        if (opened) {
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
            auto ql = vdec->queueLock(qf, 0);
            if (gpu->initAdopt(inst, phys, dev, qf, qflags,
                               {std::move(ql.lock), std::move(ql.unlock), vdec->deviceKeepAlive()},
                               gerr, lensFormat, vdec->lens(0).ycbcr)) {
                vdec_ = std::move(vdec);
                gpu_ = std::move(gpu);
                const auto& lenses = lens_sources_[0].lenses;
                for (int i = 0; i < 2 && i < static_cast<int>(lenses.size()); ++i) {
                    const orbiscore::LensRect& r = lenses[i].rect;
                    gpu_params_.lensRect[i][0] = static_cast<float>(r.x);
                    gpu_params_.lensRect[i][1] = static_cast<float>(r.y);
                    gpu_params_.lensRect[i][2] = static_cast<float>(r.w);
                    gpu_params_.lensRect[i][3] = static_cast<float>(r.h);
                }
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
    auto demux = std::make_unique<orbiscore::FisheyeDemux>();
    std::string err;
    if (!demux->open(lens_sources_, err)) {
        last_error_ = "demux open failed: " + err + ": " + clip_path_;
        return false;
    }
    if (!std::getenv("ORBIS_NO_GPU_STITCH")) {
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
    gpu_fallback_.stitch = !gpu_ && !std::getenv("ORBIS_NO_GPU_STITCH");
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
    blur_refused_ = false;
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    const int64_t t = timestamp_ms < 0 ? 0 : timestamp_ms;
    // Wanted until the seek is done: a read after one that failed seeks
    // here again first, never reads on from where the decoder was.
    resume_seek_ = true;
    resume_at_ms_ = t;
    if (!ResumeGpu())
        return false;
    const bool quick = quick_seek_;
    quick_seek_ = false;
    scrub_frame_ = quick;
    seam_window_.reset(); // the frames measured so far are about another stretch
    if (!quick)           // a scrub's frame keeps the last frame's seam
        seam_state_ = orbiscore::SeamState{};
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
    if (!seekDecoder(t))
        return false;
    resume_seek_ = false;
    return true;
}

bool NativeStitchSource::preRollSeam(int64_t t) {
    // The seam colour blend pools the quarter second before a frame, the seam flow
    // the few frames before it: measure them now, so the frame at t is
    // stitched as playback into it has it. Every frame before the one the
    // seek below lands on (the first at or after t) counts, so read on
    // until a frame at or after t; that one is read again after the seek.
    // The flow's frames are pictures: in proxy mode the preview file's, as
    // playing on measures them (each once, not its repeats).
    const double frameMs = proxy_                 ? proxy_->proxyFrameUs * 1e-3
                           : info_.fps > 0 ? 1000.0 / info_.fps
                                           : 33.4;
    const int64_t flowFrom =
        seam_flow_on_ ? t - std::llround((orbiscore::kSeamFlowFrames - 0.5) * frameMs) : INT64_MAX;
    int64_t from = t;
    if (seam_colour_on_)
        from = t - seam_colour_window_.lookBackMs();
    from = std::min(from, flowFrom);
    if (!seekDecoder(std::max<int64_t>(0, from)))
        return false;
    for (;;) {
        int64_t pts = 0;
        bool colourMeasured = false;
        bool flowMeasured = false;
        if (!measureNextSeamFrame(pts, flowFrom, t, &colourMeasured, &flowMeasured)) {
            if (!last_error_.empty())
                return false; // not the frame playing into t would show
            break;            // the clip ends before t: the frames so far
        }
        if (pts >= t)
            break; // the seek's own frame: measured when it's read
        if (colourMeasured)
            seam_colour_window_.add(pts, *cut_stats_, gpu_params_.bandRad);
        if (flowMeasured)
            addSeamFlow();
    }
    return true;
}

bool NativeStitchSource::addSeamFlow() {
    if (!seam_flow_estimator_ || !seam_flow_strips_)
        return false;
    orbiscore::SeamFlowField f;
    if (!seam_flow_estimator_->estimate(*seam_flow_strips_, f))
        return false;
    if (seam_colour_on_) {
        orbiscore::SeamFadeFrame fade;
        orbiscore::MeasureSeamFadeFrame(*seam_flow_strips_, f, fade);
        seam_fade_window_.add(std::move(fade));
    }
    seam_flow_window_.add(std::move(f));
    auto solved = std::make_shared<orbiscore::SeamFlowField>();
    seam_flow_window_.solve(*solved);
    seam_flow_field_ = std::move(solved);
    return true;
}

bool NativeStitchSource::seekDecoder(int64_t t) {
    std::string err;
    if (proxy_)
        t = proxy_->seekTo(t);
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
    orbiscore::FramePts pts;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = readPairVk(f0, f1, &pts, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return Frame{};
    }
    resume_at_ms_ = pts.ms + 1; // a resume carries on after it

    orbiscore::VulkanLensFrame lf0, lf1;
    vdec_->lockPair(f0, f1, lf0, lf1);

    Frame frame;
    frame.width = eq_w_;
    frame.height = eq_h_;
    frame.timestamp_ms = pts.ms;
    frame.timestamp_us = pts.us;
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    orbiscore::StitchGpuParams params = paramsWithSeam(pts, in0, in1);
    std::string blurErr;
    const bool blurred = blurQuadsAt(pts, params, blurErr);
    const bool ok =
        blurred && gpu_->stitchVk(in0, in1, params, eq_w_, eq_h_, frame.rgba, &oc0, &oc1, gerr);

    orbiscore::VulkanLensFrameResult r0{oc0.semSignalValue, oc0.newLayout};
    orbiscore::VulkanLensFrameResult r1{oc1.semSignalValue, oc1.newLayout};
    if (ok) {
        vdec_->unlockPair(f0, f1, r0, r1);
    } else {
        // On failure the frames are as the seam measure (if any) left them.
        vdec_->unlockPair(f0, f1, {in0.semWaitValue, in0.layout}, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);

    if (!blurred) {
        refuseFrame(std::move(blurErr));
        return refusedFrame(pts);
    }
    if (!ok) {
        last_error_ = "GPU stitchVk failed: " + gerr;
        return Frame{};
    }
    copyGpuTiming();
    return frame;
}

void NativeStitchSource::SetSeamCorrection(const orbiscore::SeamCorrection& c) {
    if (!stitcher_ || c == params_.seam || method_ == PreviewStitchConfig::Method::New)
        return;
    params_.seam = c;
    if (stitcher_->configure(calib_, lens_w_, lens_h_, params_)) {
        keepLensRect(gpu_params_, stitcher_->gpuParams()); // RS fields: set in paramsAt
        seam_window_.configure(stitcher_->baselineM(), gpu_params_.bandRad); // frames measured on the old one go
    }
    analysis_dirty_ = true;
}

void NativeStitchSource::SetLensColour(const orbiscore::LensColour& c) {
    if (!stitcher_ || c == params_.lensColour || method_ == PreviewStitchConfig::Method::New)
        return;
    params_.lensColour = c;
    seam_window_.setLensColour(c); // the frames' residuals are measured against it
    analysis_dirty_ = true;
    if (stitcher_->configure(calib_, lens_w_, lens_h_, params_))
        keepLensRect(gpu_params_, stitcher_->gpuParams());
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

void NativeStitchSource::ConfigureSeamWindow(orbiscore::SeamWindow& w) const {
    if (!stitcher_)
        return;
    w.configure(stitcher_->baselineM(), gpu_params_.bandRad);
    w.setLensColour(params_.lensColour);
}

bool NativeStitchSource::MeasureNextSeamFrame(orbiscore::SeamWindow& w, int64_t& pts_ms) {
    last_error_.clear();
    blur_refused_ = false;
    if (!resumeForRead())
        return false;
    float q[orbiscore::kSeamDepthBins];
    orbiscore::FramePts pts;
    if (vdec_) {
        ReleaseHeldPair();
        AVFrame* f0 = nullptr;
        AVFrame* f1 = nullptr;
        std::string err;
        if (!readPairVk(f0, f1, &pts, err)) {
            last_error_ = err; // empty at EOS
            return false;
        }
        resume_at_ms_ = pts.ms + 1; // a resume carries on after it
        orbiscore::VulkanLensFrame lf0, lf1;
        vdec_->lockPair(f0, f1, lf0, lf1);
        render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
        orbiscore::StitchGpuParams p = gpu_params_;
        pts_ms = pts.ms;
        w.measureTable(pts_ms, q);
        std::copy(q, q + orbiscore::kSeamDepthBins, p.seamQ);
        render::VulkanLensOutcome o0, o1;
        const bool ok = gpu_->measureSeamVk(in0, in1, p, *seam_costs_, cut_stats_.get(), &o0, &o1, err);
        if (ok) {
            vdec_->unlockPair(f0, f1, {o0.semSignalValue, o0.newLayout},
                              {o1.semSignalValue, o1.newLayout});
        } else {
            vdec_->unlockPair(f0, f1, {in0.semWaitValue, in0.layout},
                              {in1.semWaitValue, in1.layout});
        }
        vdec_->freeFrame(f0);
        vdec_->freeFrame(f1);
        if (!ok) {
            last_error_ = "seam measure failed: " + err;
            return false;
        }
    } else if (demux_ && stitcher_) {
        orbiscore::Image a, b;
        std::string err;
        if (!readPairCpu(a, b, &pts, err)) {
            last_error_ = err;
            return false;
        }
        resume_at_ms_ = pts.ms + 1; // a resume carries on after it
        pts_ms = pts.ms;
        w.measureTable(pts_ms, q);
        orbiscore::MeasureSeamCutStats(*stitcher_, a, b, q, *cut_stats_);
        orbiscore::MeasureSeamCosts(*stitcher_, a, b, *seam_costs_);
    } else {
        last_error_ = "MeasureNextSeamFrame before a successful Open";
        return false;
    }
    w.add(pts_ms, *seam_costs_, *cut_stats_);
    return true;
}

orbiscore::StitchGpuParams NativeStitchSource::paramsAt(const orbiscore::FramePts& pts) const {
    orbiscore::StitchGpuParams p = gpu_params_;
    p.blur.clear();
    CaptureTiming ct;
    {
        std::lock_guard<std::mutex> lk(rs_mx_);
        // copy: call rotationAt outside the lock
        ct = pts.ms < capture_until_ms_ ? capture_timing_ : capture_after_;
    }
    if (!ct.active())
        return p;
    const double t = pts.sec();
    constexpr int kTaps = orbiscore::StitchGpuParams::kRsTaps;
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

bool NativeStitchSource::blurQuadsAt(const orbiscore::FramePts& pts, orbiscore::StitchGpuParams& p,
                                     std::string& err) const {
    p.blur.clear();
    BlurQuadsFn blur;
    {
        std::lock_guard<std::mutex> lk(rs_mx_);
        blur = blur_quads_;
    }
    if (!blur)
        return true;
    std::vector<orbiscore::BlurQuad> quads;
    blur(pts.ms, quads);
    if (orbiscore::prepareBlurQuads(quads, p.blur))
        return true;
    // Never some of them: the ones left out would show what they cover.
    const long long ms = pts.ms;
    char at[32];
    std::snprintf(at, sizeof at, "%lld:%02lld.%03lld", ms / 60000, ms / 1000 % 60, ms % 1000);
    err = std::to_string(p.blur.size()) + " privacy regions at " + at +
          " in the file; the blur takes at most " + std::to_string(orbiscore::kMaxBlurQuads) +
          " in one frame";
    p.blur.clear();
    return false;
}

void NativeStitchSource::refuseFrame(std::string err) {
    last_error_ = std::move(err);
    blur_refused_ = true;
}

Frame NativeStitchSource::refusedFrame(const orbiscore::FramePts& pts) {
    Frame f;
    f.timestamp_ms = pts.ms;
    f.timestamp_us = pts.us;
    return f;
}

IStitchSource::ReadFailure NativeStitchSource::GetLastFailure() const {
    if (blur_refused_)
        return ReadFailure::TooManyBlurRegions;
    return last_error_.empty() ? ReadFailure::None : ReadFailure::Error;
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
    float q[orbiscore::kSeamDepthBins];
    seam_window_.measureTable(pts_ms, q);
    if (!measure(q))
        return false;
    seam_window_.add(pts_ms, *seam_costs_, *cut_stats_);
    seam_window_.solve(pts_ms, seam_state_);
    seam_window_.dropBefore(pts_ms - seam_window_.lookBackMs());
    return true;
}

orbiscore::StitchGpuParams NativeStitchSource::paramsWithSeam(const orbiscore::FramePts& pts,
                                                              render::VulkanLensInput& in0,
                                                              render::VulkanLensInput& in1) {
    orbiscore::StitchGpuParams p = paramsAt(pts);
    seam_ms_ = 0.0;
    // A repeat of the frame before's picture: the new stitcher's measures of
    // it are there already. The depth-aware seam (the old stitcher) measures
    // every frame, as on the full file.
    const bool quickScrub = scrub_frame_;
    const bool scrub = quickScrub || (picture_repeat_ && !p.depthActive);
    scrub_frame_ = false;
    if (scrub) {
        // A scrub's passing frame: no seam measurement, the last frame's
        // seam (the depth seam's from its analysis where it has the frame).
        if (seam_colour_on_)
            repeatSeamColour(pts.ms, p, picture_repeat_ && !quickScrub);
        if (seam_flow_on_)
            p.seamFlowGpu = seam_flow_gpu_;
        if (p.depthActive) {
            seamStateFor(pts.ms, [](const float*) { return false; });
            std::copy(seam_state_.q, seam_state_.q + orbiscore::kSeamDepthBins, p.seamQ);
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
            seamMeasureFailed(err);
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
        orbiscore::StitchGpuParams m = p;
        std::copy(q, q + orbiscore::kSeamDepthBins, m.seamQ);
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
    std::copy(seam_state_.q, seam_state_.q + orbiscore::kSeamDepthBins, p.seamQ);
    p.seamBlend = seam_state_.blend;
    seam_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                   .count();
    return p;
}

void NativeStitchSource::heldSeamColour(orbiscore::StitchGpuParams& p) const {
    p.seamColourActive = 1;
    std::copy(&seam_colour_table_[0][0], &seam_colour_table_[0][0] + orbiscore::kSeamDepthBins * 3,
              &p.seamColour[0][0]);
    std::copy(&seam_veil_[0][0], &seam_veil_[0][0] + orbiscore::kSeamDepthBins * 3,
              &p.seamVeil[0][0]);
}

void NativeStitchSource::repeatSeamColour(int64_t pts_ms, orbiscore::StitchGpuParams& p,
                                          bool solve) {
    // A repeat of the frame before's picture is solved at its own time from
    // the frames measured, as playing on and a seek's pre-roll (which measures
    // but doesn't solve) both have them; a scrub's frame keeps the last table.
    if (solve) {
        seam_colour_window_.solve(pts_ms, &seam_colour_table_[0][0], &seam_veil_[0][0]);
        seam_colour_window_.dropBefore(pts_ms - seam_colour_window_.lookBackMs());
    }
    heldSeamColour(p);
}

bool NativeStitchSource::applySeamColour(int64_t pts_ms, orbiscore::StitchGpuParams& p,
                                         const std::function<bool()>& measure) {
    if (!measure())
        return false;
    seam_colour_window_.add(pts_ms, *cut_stats_, gpu_params_.bandRad);
    seam_colour_window_.solve(pts_ms, &seam_colour_table_[0][0], &seam_veil_[0][0]);
    seam_colour_window_.dropBefore(pts_ms - seam_colour_window_.lookBackMs());
    heldSeamColour(p);
    return true;
}

void NativeStitchSource::seamMeasureFailed(const std::string& err) {
    if (!seam_warned_) {
        seam_warned_ = true;
        std::fprintf(stderr, "[NativeStitchSource] seam %s measure failed (%s); "
                             "stitching without it\n",
                     seam_flow_on_ ? "flow" : "colour", err.c_str());
    }
    // Without the flow from the next frame on, so the colour blend still
    // measures.
    seam_flow_on_ = false;
    seam_flow_gpu_ = false;
}

bool NativeStitchSource::measureNextSeamFrame(int64_t& pts_ms, int64_t flowFrom, int64_t until,
                                              bool* colourMeasured, bool* flowMeasured) {
    last_error_.clear();
    *colourMeasured = false;
    *flowMeasured = false;
    orbiscore::FramePts pts;
    if (vdec_) {
        ReleaseHeldPair();
        AVFrame* f0 = nullptr;
        AVFrame* f1 = nullptr;
        std::string err;
        if (!readPairVk(f0, f1, &pts, err)) {
            last_error_ = err; // empty at EOS
            return false;
        }
        pts_ms = pts.ms;
        orbiscore::VulkanLensFrame lf0, lf1;
        vdec_->lockPair(f0, f1, lf0, lf1);
        render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
        render::VulkanLensOutcome o0, o1;
        // Measured as playback measures this frame: with its capture-time
        // correction, which moves what the strips see.
        const orbiscore::StitchGpuParams p = paramsAt(pts);
        const bool measure = pts_ms < until && !picture_repeat_; // a repeat: measured already
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
        *colourMeasured = ok && measure && seam_colour_on_;
        if (ok) {
            vdec_->unlockPair(f0, f1, {o0.semSignalValue, o0.newLayout},
                              {o1.semSignalValue, o1.newLayout});
        } else {
            vdec_->unlockPair(f0, f1, {in0.semWaitValue, in0.layout},
                              {in1.semWaitValue, in1.layout});
        }
        vdec_->freeFrame(f0);
        vdec_->freeFrame(f1);
        if (!ok)
            seamMeasureFailed(err); // as playing into the frame has it
    } else if (demux_ && stitcher_) {
        orbiscore::Image a, b;
        std::string err;
        if (!readPairCpu(a, b, &pts, err)) {
            last_error_ = err;
            return false;
        }
        pts_ms = pts.ms;
        if (pts_ms >= until || picture_repeat_) // a repeat: measured already
            return true;
        const float none[orbiscore::kSeamDepthBins] = {};
        if (seam_colour_on_) {
            orbiscore::MeasureSeamCutStats(*stitcher_, a, b, none, *cut_stats_);
            *colourMeasured = true;
        }
        if (seam_flow_on_ && pts_ms >= flowFrom) {
            if (gpu_) {
                // Measured as playback measures this frame (with its
                // capture-time correction).
                if (gpu_->measureSeamFlow(a.rgb.data(), b.rgb.data(), paramsAt(pts), err))
                    seam_flow_gpu_ = true;
                else
                    seamMeasureFailed(err);
            } else {
                orbiscore::MeasureSeamFlowStrips(*stitcher_, seam_flow_grid_, a, b,
                                                 *seam_flow_strips_);
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
    if (!stitcher_ || lens_h_ <= 0)
        return false;
    const orbiscore::Mat3& R = stitcher_->outRotation(); // stitch world = outRot * eqDir
    const orbiscore::Vec3 w{R.m[0][0] * eqDir[0] + R.m[0][1] * eqDir[1] + R.m[0][2] * eqDir[2],
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
    rowFrac = v[lens] / static_cast<double>(lens_h_);
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
        [pose, stab](const orbiscore::FramePts&, view::Pose& p, float s[9]) {
            if (pose)
                p = *pose;
            if (stab)
                std::copy(stab, stab + 9, s);
        },
        colorGrade);
}

FlatFrame NativeStitchSource::ReadFlatFrame(int outW, int outH, const FramePoseFn& perFrame,
                                            const color::Grade* colorGrade) {
    blur_refused_ = false;
    if (!resumeForRead())
        return FlatFrame{};
    if (!GpuReframeAvailable()) {
        last_error_ = "ReadFlatFrame without the all-GPU reproject path";
        return FlatFrame{};
    }
    // The fused pass unless it's missing or ORBIS_FLAT_VIA_EQUIRECT asks for
    // the stitch to an equirect and a reproject from it (for comparisons).
    const bool fused = GpuViewAvailable() &&
                       (!gpu_->reframeReady() || !std::getenv("ORBIS_FLAT_VIA_EQUIRECT"));
    FlatFrame frame;
    orbiscore::FramePts pts;
    frame.p010 = fused && flat_ten_bit_;
    if (!readView(outW, outH, perFrame, colorGrade,
                  !fused         ? FlatPath::ViaEquirect
                  : frame.p010   ? FlatPath::FusedP010
                                 : FlatPath::FusedNv12,
                  frame.data, pts))
        return FlatFrame{};
    frame.timestamp_ms = pts.ms;
    frame.timestamp_us = pts.us;
    frame.width = outW;
    frame.height = outH;
    return frame;
}

Frame NativeStitchSource::ReadViewFrame(int outW, int outH, const FramePoseFn& perFrame,
                                        const color::Grade* colorGrade) {
    blur_refused_ = false;
    if (!resumeForRead())
        return Frame{};
    if (!GpuViewAvailable()) {
        last_error_ = "ReadViewFrame without the fused GPU view path";
        return Frame{};
    }
    Frame frame;
    orbiscore::FramePts pts;
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
                                  std::vector<uint8_t>& out, orbiscore::FramePts& pts) {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    pts = {};
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = readPairVk(f0, f1, &pts, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return false;
    }
    resume_at_ms_ = pts.ms + 1; // a resume carries on after it

    orbiscore::VulkanLensFrame lf0, lf1;
    vdec_->lockPair(f0, f1, lf0, lf1);

    view::Pose pose; // forward view, the reproject's default
    float stab[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (perFrame)
        perFrame(pts, pose, stab);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    orbiscore::StitchGpuParams params = paramsWithSeam(pts, in0, in1);
    std::string blurErr;
    const bool blurred = blurQuadsAt(pts, params, blurErr);
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
    const bool ok = blurred &&
        (!views.empty()
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
                                 fmt, out, &oc0, &oc1, gerr));

    orbiscore::VulkanLensFrameResult r0{oc0.semSignalValue, oc0.newLayout};
    orbiscore::VulkanLensFrameResult r1{oc1.semSignalValue, oc1.newLayout};
    if (ok) {
        vdec_->unlockPair(f0, f1, r0, r1);
    } else {
        vdec_->unlockPair(f0, f1, {in0.semWaitValue, in0.layout}, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);

    if (!blurred) {
        refuseFrame(std::move(blurErr));
        return false;
    }
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
    orbiscore::StitchGpuParams params;
    orbiscore::FramePts pts;
};

int NativeStitchSource::KeepNextPair(orbiscore::FramePts& pts) {
    blur_refused_ = false;
    if (!resumeForRead())
        return 0;
    if (!GpuViewAvailable()) {
        last_error_ = "KeepNextPair without the fused view path";
        return 0;
    }
    auto h = std::make_unique<HeldPair>();
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = readPairVk(h->f0, h->f1, &h->pts, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return 0;
    }
    resume_at_ms_ = h->pts.ms + 1; // a resume carries on after it
    orbiscore::VulkanLensFrame lf0, lf1;
    vdec_->lockPair(h->f0, h->f1, lf0, lf1);
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    h->params = paramsWithSeam(h->pts, in0, in1);
    vdec_->unlockPair(h->f0, h->f1, {in0.semWaitValue, in0.layout}, {in1.semWaitValue, in1.layout});
    std::string blurErr;
    if (!blurQuadsAt(h->pts, h->params, blurErr)) {
        vdec_->freeFrame(h->f0);
        vdec_->freeFrame(h->f1);
        refuseFrame(std::move(blurErr));
        return 0;
    }
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
    orbiscore::VulkanLensFrame lf0, lf1;
    vdec_->lockPair(h.f0, h.f1, lf0, lf1);
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    const bool ok = gpu_->renderViewsVk(in0, in1, h.params, canvasW, canvasH, vr, stab,
                                        clearRgba, rgba, &oc0, &oc1, gerr);
    if (ok) {
        vdec_->unlockPair(h.f0, h.f1, {oc0.semSignalValue, oc0.newLayout},
                          {oc1.semSignalValue, oc1.newLayout});
        copyGpuTiming();
        return true;
    }
    vdec_->unlockPair(h.f0, h.f1, {in0.semWaitValue, in0.layout}, {in1.semWaitValue, in1.layout});
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

bool NativeStitchSource::HoldNextPair(orbiscore::FramePts& pts) {
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
    return vdec_ && gpu_ && gpu_->lensExportReady() && !std::getenv("ORBIS_PLAYBACK_EQUIRECT");
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
    if (proxy_)
        proxy_->drop(); // its frames are the decoder's
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
    // Where the source was: the frame after the last one read, or the
    // target of a seek that failed. SeekTo clears the flag once it has
    // worked; its pre-roll measures the seam into that frame as playing on
    // would have.
    return SeekTo(resume_at_ms_);
}

bool NativeStitchSource::ReadLensFrame(render::LensFrame& out) {
    blur_refused_ = false;
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    if (!resumeForRead())
        return false;
    if (!LensFramesAvailable()) {
        last_error_ = "ReadLensFrame without the all-GPU lens export path";
        return false;
    }
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    orbiscore::FramePts pts;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = readPairVk(f0, f1, &pts, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return false;
    }
    resume_at_ms_ = pts.ms + 1; // a resume carries on after it

    orbiscore::VulkanLensFrame lf0, lf1;
    vdec_->lockPair(f0, f1, lf0, lf1);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    orbiscore::StitchGpuParams params = paramsWithSeam(pts, in0, in1);
    std::string blurErr;
    const bool blurred = blurQuadsAt(pts, params, blurErr);
    const bool ok = blurred && gpu_->fillLensFrameVk(in0, in1, params, out, &oc0, &oc1, gerr);
    if (ok) {
        vdec_->unlockPair(f0, f1, {oc0.semSignalValue, oc0.newLayout},
                          {oc1.semSignalValue, oc1.newLayout});
    } else {
        vdec_->unlockPair(f0, f1, {in0.semWaitValue, in0.layout}, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);
    if (!blurred) {
        // Not decoded again: the next read goes on past it.
        refuseFrame(std::move(blurErr));
        out = render::LensFrame{};
        out.timestamp_ms = pts.ms;
        out.timestamp_us = pts.us;
        return false;
    }
    if (!ok) {
        // Most often the graphics memory is full for the moment: the next
        // read decodes this pair again rather than going on without it.
        last_error_ = "GPU fillLensFrameVk failed: " + gerr;
        resume_at_ms_ = pts.ms;
        resume_seek_ = true;
        return false;
    }
    out.timestamp_ms = pts.ms;
    out.timestamp_us = pts.us;
    copyGpuTiming();
    return true;
}

Frame NativeStitchSource::readCpuDecode() {
    orbiscore::Image a, b;
    orbiscore::FramePts pts;
    std::string err;
    if (!readPairCpu(a, b, &pts, err)) {
        last_error_ = err; // empty on a clean end-of-stream
        return Frame{};
    }
    resume_at_ms_ = pts.ms + 1; // a resume carries on after it

    // A scrub's passing frame is stitched with the last frame's seam, as the
    // GPU path.
    const bool quickScrub = scrub_frame_;
    const bool scrub = quickScrub || (picture_repeat_ && !params_.depthSeam); // as the GPU path
    scrub_frame_ = false;
    // Depth-aware seam: the cost volume on the CPU here (no GPU-resident
    // lenses to run the GPU pass on).
    if (params_.depthSeam) {
        const auto t0 = std::chrono::steady_clock::now();
        seamStateFor(pts.ms, [&](const float* q) { // as the GPU path
            if (scrub)
                return false;
            orbiscore::MeasureSeamCutStats(*stitcher_, a, b, q, *cut_stats_);
            orbiscore::MeasureSeamCosts(*stitcher_, a, b, *seam_costs_);
            return true;
        });
        last_timing_.seamMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
    }
    const float* seamQ = params_.depthSeam ? seam_state_.q : nullptr;
    const orbiscore::SeamBlend* blend = params_.depthSeam ? &seam_state_.blend : nullptr;
    // The seam colour blend: the lenses' colour in the overlap at the
    // template's geometry, as the GPU path.
    orbiscore::StitchGpuParams p = paramsAt(pts);
    if (seam_colour_on_ && scrub) {
        repeatSeamColour(pts.ms, p, picture_repeat_ && !quickScrub);
    } else if (seam_colour_on_) {
        const auto t0 = std::chrono::steady_clock::now();
        applySeamColour(pts.ms, p, [&] {
            const float none[orbiscore::kSeamDepthBins] = {};
            orbiscore::MeasureSeamCutStats(*stitcher_, a, b, none, *cut_stats_);
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
            orbiscore::MeasureSeamFlowStrips(*stitcher_, seam_flow_grid_, a, b, *seam_flow_strips_);
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
            auto f = std::make_shared<orbiscore::SeamFlowField>(*seam_flow_field_);
            seam_fade_window_.solve(&p.seamColour[0][0], f->fade);
            p.seamFlow = std::move(f);
        }
    }

    // Measured as for any frame (as the GPU path), then the blur.
    std::string blurErr;
    if (!blurQuadsAt(pts, p, blurErr)) {
        refuseFrame(std::move(blurErr));
        return refusedFrame(pts);
    }
    if (gpu_) {
        Frame f;
        f.width = eq_w_;
        f.height = eq_h_;
        f.timestamp_ms = pts.ms;
        f.timestamp_us = pts.us;
        std::string gerr;
        if (seamQ) {
            std::copy(seamQ, seamQ + orbiscore::kSeamDepthBins, p.seamQ);
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

    orbiscore::Image eq;
    eq.alloc(eq_w_, eq_h_);
    stitcher_->renderEquirect(a, b, eq, seamQ, blend, p.blur.data(), static_cast<int>(p.blur.size()),
                              p.seamColourActive ? &p.seamColour[0][0] : nullptr, p.seamFlow.get(),
                              p.seamColourActive ? &p.seamVeil[0][0] : nullptr);
    return ToRgbaFrame(eq, pts);
}

Frame NativeStitchSource::ReadFrame() {
    blur_refused_ = false;
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
