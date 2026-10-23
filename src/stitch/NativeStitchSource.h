#pragma once

#include "IStitchSource.h"

#include <ClipMetadata.h>      // camera::ClipMetadata

#include <ColorLut.h>          // color::Grade
#include <ViewProjection.h>    // view::Pose
#include <FisheyeProjection.h> // insv::StitchGpuParams (value member below)
#include <SeamDepth.h>         // insv::SeamDepthTracker (value member below)

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

struct AVFrame;

namespace insv {
class InsvDemux;
class FisheyeStitcher;
class VulkanVideoDecoder;
} // namespace insv

namespace render {
class VulkanFisheyeStitcher;
struct VulkanLensInput;
} // namespace render

namespace stitch {

// One reprojected rectilinear ("flat") frame, tightly packed: NV12 (Y plane
// width*height bytes, then interleaved UV width*height/2), or with `p010`
// the same layout in 16-bit little-endian samples, the 10-bit code in the
// high bits. Produced entirely on the GPU by NativeStitchSource::ReadFlatFrame
// -- no CPU reproject / swscale.
struct FlatFrame {
    std::vector<uint8_t> data;
    bool p010 = false;
    int width = 0;
    int height = 0;
    int64_t timestamp_ms = 0;
};

// Where the time went in the last all-GPU ReadFrame() / ReadFlatFrame()
// (tools/playback_bench). Mirrors render::StitchTiming plus the decode hand-off.
struct ReadTiming {
    double decodeMs = 0.0;      // readNextPair: waiting for the decoder to hand over a pair
    double fenceWaitMs = 0.0;   // submit -> fence (decode completion + stitch + reproject)
    double stitchGpuMs = -1.0;  // GPU timestamps; < 0 if unavailable
    double reframeGpuMs = -1.0; // ReadFlatFrame only
    double copyMs = 0.0;        // memcpy out of the host-visible buffer
    double seamMs = 0.0;        // depth-aware seam: cost pass + solve (0 when off)
};

// IStitchSource implemented entirely on the cleanroom src/insv/ module:
// the clip's description (camera::ReadClipMetadata) -> insv::InsvDemux (2 lens streams) ->
// insv::FisheyeStitcher, all in-process, with true frame-at-time-T random
// access. This is the only IStitchSource backend.
//
// Quality: geometry + feather blend + photometric seam compensation + pole
// handling (see FisheyeProjection.h), plus the depth-aware seam
// (PreviewStitchConfig::depthSeam): every frame the seam's cost volume is
// measured first (render::VulkanFisheyeStitcher::measureSeamVk, or
// insv::MeasureSeamCosts on the CPU path), insv::SeamDepthTracker solves the
// per-azimuth stitch distance, and that frame is stitched with it. Of
// PreviewStitchConfig, width/height (the equirect output size), lensGuard (the
// trailer lens profile) and depthSeam are honoured; quality is inert (no
// optical-flow seam yet). Stabilization is applied at reprojection time
// (insv::StabConfig), never here.
//
// Open() is cheap (trailer parse + demux open, no stitching). Frames are
// produced on demand in SeekTo/ReadFrame/GetFrame. Not safe to call
// GetFrame/SeekTo/ReadFrame concurrently with each other.
class NativeStitchSource : public IStitchSource {
public:
    NativeStitchSource();
    ~NativeStitchSource() override;

    NativeStitchSource(const NativeStitchSource&) = delete;
    NativeStitchSource& operator=(const NativeStitchSource&) = delete;

    bool Open(const std::string& clip_path, const PreviewStitchConfig& config) override;
    const ClipInfo& GetInfo() const override { return info_; }
    Frame GetFrame(int64_t timestamp_ms) override;
    bool SeekTo(int64_t timestamp_ms) override;
    int64_t NearestKeyframeMs(int64_t timestamp_ms) const override;
    void KeyframesAround(int64_t timestamp_ms, int64_t& before, int64_t& after) const override;
    Frame ReadFrame() override;
    const std::string& GetLastError() const override { return last_error_; }

    // True when ReadFlatFrame() can run: the all-GPU path is active and either
    // the fused view pipeline or the reproject pipeline built. Otherwise
    // callers must reproject the equirect from ReadFrame() on the CPU.
    bool GpuReframeAvailable() const;

    // True when the fused fisheye -> view pass is available (ReadViewFrame,
    // and what ReadFlatFrame uses unless INSV_FLAT_VIA_EQUIRECT is set).
    bool GpuViewAvailable() const;

    // Reads the next lens pair and renders it on the GPU to an outW x outH
    // rectilinear NV12 frame against `pose` -- straight from the fisheyes
    // (fused pass, one resample) when GpuViewAvailable(), else via a GPU
    // equirect + reproject. `pose` nullptr => the forward view. `stab` (row-major 3x3, nullptr => identity) is the
    // frame's gyro de-rotation in the sphere frame, applied after `pose` like
    // the viewport. `colorGrade` (nullptr => neutral) is the Colour
    // panel grade, its LUT included. outW must
    // be a multiple of 4, outH a multiple of 2. Advances the playhead like
    // ReadFrame(). Returns an empty FlatFrame (data.empty()) at end-of-clip;
    // also on error, with GetLastError() set. Requires GpuReframeAvailable().
    // NV12 unless SetFlatTenBit(true) asked for P010 and the fused pass runs
    // (the equirect is 8-bit, so the via-equirect path stays NV12):
    // FlatFrame::p010 says which.
    FlatFrame ReadFlatFrame(int outW, int outH, const view::Pose* pose,
                            const float stab[9] = nullptr,
                            const color::Grade* colorGrade = nullptr);

    // As above, but the pose and stab matrix come from `perFrame(ptsMs, pose,
    // stab)`, called once the next frame is decoded and its source time is
    // known -- so they are evaluated at that frame's own time. A caller that
    // bakes them from an output index instead (trimIn + i/fps) is off by up to
    // a frame whenever trimIn isn't on a frame boundary: SeekTo lands on the
    // first frame at-or-after it. At the stick mount's ~11 Hz shake that is a
    // phase error large enough to undo most of the stabilization. `pose` is
    // prefilled with a forward view and `stab` with identity.
    using FramePoseFn = std::function<void(int64_t ptsMs, view::Pose& pose, float stab[9])>;
    FlatFrame ReadFlatFrame(int outW, int outH, const FramePoseFn& perFrame,
                            const color::Grade* colorGrade = nullptr);

    // ReadFlatFrame renders P010 instead of NV12 where it can (the 10-bit
    // export). Off by default.
    void SetFlatTenBit(bool on) { flat_ten_bit_ = on; }

    // Split screen: the frame ReadFlatFrame / ReadViewFrame render divided
    // into views, its panes -- each its rectangle in the frame (x and w
    // multiples of 4, y and h of 2) and its own pose. The first pane's pose
    // (and every pane's stab) comes from the read's perFrame as usual, the
    // others' from `poseAt(ptsMs, pane, pose)` at the same frame time
    // (prefilled with a forward view). What no pane covers is `fillRgba` (R
    // in the low byte). The panes are for the outW x outH the reads ask
    // for; fewer than two = one view filling the frame (the default).
    struct ViewPane {
        int x = 0, y = 0, w = 0, h = 0;
    };
    using PanePoseFn = std::function<void(int64_t ptsMs, int pane, view::Pose& pose)>;
    void SetViewPanes(std::vector<ViewPane> panes, PanePoseFn poseAt, uint32_t fillRgba);

    // The fused pass with RGBA8 output (Frame::rgba, outW x outH): the view
    // as a picture rather than an encoder input -- stills, and the viewport
    // later on. Same pose / stab / grade handling as ReadFlatFrame. Requires
    // GpuViewAvailable(); empty Frame at end-of-clip or on error.
    Frame ReadViewFrame(int outW, int outH, const FramePoseFn& perFrame,
                        const color::Grade* colorGrade = nullptr);

    // Several views of one frame, rendered when the caller is ready (the
    // privacy blur's plate scan: the detector's tiles first, the quad
    // tracker's views once it knows where to look). HoldNextPair decodes the
    // next frame and keeps it; RenderHeldViews renders `views` of it into one
    // canvasW x canvasH RGBA8 canvas (cleared to `clearRgba`; see
    // render::VulkanFisheyeStitcher::renderViewsVk), as often as wanted, with
    // `stab` (the sphere-frame rotation after each pose); ReleaseHeldPair (or
    // the next HoldNextPair / Read*) lets it go. Requires GpuViewAvailable().
    // False at end-of-clip (GetLastError() empty) or on error.
    struct ViewRect {
        view::Pose pose;
        int x = 0, y = 0, w = 0, h = 0;
    };
    bool HoldNextPair(int64_t& pts_ms);
    bool RenderHeldViews(int canvasW, int canvasH, const std::vector<ViewRect>& views,
                         const float stab[9], uint32_t clearRgba, std::vector<uint8_t>& rgba);
    void ReleaseHeldPair();

    // Any number of decoded pairs, kept while the decoder moves on (and
    // across SeekTo) until released -- to hand frames out in reverse
    // (following a plate backwards: decode a stretch forward, then use it
    // from its end). KeepNextPair decodes the next frame and returns its
    // handle (> 0), 0 at end-of-clip (GetLastError() empty) or on error;
    // RenderKeptViews is RenderHeldViews on one of them. Each pair stays in
    // GPU memory (~44 MB at 8K) until ReleaseKeptPair, Open or destruction.
    // Requires GpuViewAvailable().
    int KeepNextPair(int64_t& pts_ms);
    bool RenderKeptViews(int handle, int canvasW, int canvasH, const std::vector<ViewRect>& views,
                         const float stab[9], uint32_t clearRgba, std::vector<uint8_t>& rgba);
    void ReleaseKeptPair(int handle);
    // Bytes of GPU memory one kept pair takes (the two decoded lens images).
    size_t KeptPairBytes() const;

    // Per-lens / per-sensor-row capture-time correction in the GPU stitch
    // (insv::StitchGpuParams::rsRot), from `timing.rotationAt`
    // evaluated around each frame's video time; which of the two timings by
    // the frame's own time. The CPU stitch path ignores it.
    using IStitchSource::SetCaptureTiming;
    void SetCaptureTiming(CaptureTiming timing, int64_t untilMs, CaptureTiming after) override;

    // Re-configures the stitch geometry with the seam correction (see
    // IStitchSource::SetSeamCorrection). Cheap; a no-op when unchanged.
    void SetSeamCorrection(const insv::SeamCorrection& c) override;

    // Privacy blur (IStitchSource::SetBlurQuads): applied in paramsAt, so
    // every path -- equirect, fused view / flat, lens frames for the
    // viewport, the CPU stitch -- carries it. More than insv::kMaxBlurQuads
    // in one frame: the rest are dropped with a warning.
    bool SetBlurQuads(BlurQuadsFn fn) override;

    // Which lens and sensor row the stitched picture takes direction `eqDir`
    // (unit, equirect / sphere frame) from: lens 0/1 and the row as a fraction
    // of the fisheye height (0 = top). False outside both imaged circles and
    // inside the seam blend, where both lenses contribute. Valid after Open.
    bool SensorRowOf(const double eqDir[3], int& lens, double& rowFrac) const;

    // IStitchSource: the decoded pair in an exportable ring slot
    // (render::VulkanFisheyeStitcher::fillLensFrameVk). Available on the
    // all-GPU path unless INSV_PLAYBACK_EQUIRECT is set.
    bool LensFramesAvailable() const override;
    bool ReadLensFrame(render::LensFrame& out) override;
    void TrimLensFrames() override;

    // The depth-aware seam's current per-azimuth inverse stitch distance
    // (insv::kSeamDepthBins values, 1/m; all 0 when off or before a frame).
    const float* SeamDepthTable() const { return depth_.table(); }

    // Timing of the last all-GPU ReadFrame() / ReadFlatFrame(). Zeroes on the
    // CPU-decode path.
    const ReadTiming& LastReadTiming() const { return last_timing_; }

private:
    insv::StitchGpuParams paramsAt(int64_t pts_ms) const; // gpu_params_ + per-frame RS
    // A decoded pair, unlocked between uses (the decoder may lock it as a
    // reference frame meanwhile): locked only around each GPU submission.
    struct HeldPair;
    std::map<int, std::unique_ptr<HeldPair>> kept_; // KeepNextPair; the held pair too
    int held_ = 0;                                  // HoldNextPair's handle in kept_, 0 = none
    int next_kept_ = 1;
    // paramsAt() plus the depth-aware seam's table for this frame: measures
    // the pair's seam costs on the GPU first (advancing in0 / in1 past that
    // submission) and folds them into depth_. Just paramsAt() when it's off.
    insv::StitchGpuParams paramsWithSeam(int64_t pts_ms, render::VulkanLensInput& in0,
                                         render::VulkanLensInput& in1);
    Frame readGpuAllVulkan();  // vdec_ + gpu_->stitchVk (stage 2, all-GPU)
    Frame readCpuDecode();     // demux_ + gpu_->stitch or stitcher_ (stage 1 / CPU)
    void copyGpuTiming();      // gpu_->lastTiming() -> last_timing_
    // Shared body of ReadFlatFrame / ReadViewFrame: decode the next pair and
    // render the view into `out` (NV12 via the equirect, NV12 fused, or RGBA
    // fused). False with last_error_ set on error; false with it empty at EOS.
    enum class FlatPath { ViaEquirect, FusedNv12, FusedP010, FusedRgba };
    bool readView(int outW, int outH, const FramePoseFn& perFrame, const color::Grade* colorGrade,
                  FlatPath path, std::vector<uint8_t>& out, int64_t& pts_ms);

    // Stage 2: GPU-resident decode -> GPU stitch, nothing on the CPU. Used when
    // both halves init; else vdec_ is null and the stage-1 path runs.
    std::unique_ptr<insv::VulkanVideoDecoder> vdec_;

    std::unique_ptr<insv::InsvDemux> demux_;               // CPU decode (stage 1 / fallback)
    std::unique_ptr<insv::FisheyeStitcher> stitcher_;      // CPU reference / fallback
    std::unique_ptr<render::VulkanFisheyeStitcher> gpu_;   // GPU stitch (own device or FFmpeg's)
    insv::StitchGpuParams gpu_params_{};                   // geometry for gpu_, from stitcher_
    insv::InsvCalibration calib_;                          // what stitcher_ was configured with
    insv::StitchParams params_;
    mutable std::mutex rs_mx_; // SetCaptureTiming may come from the GUI thread
    CaptureTiming capture_timing_; // guarded by rs_mx_; frames before capture_until_ms_
    int64_t capture_until_ms_ = INT64_MAX; // guarded by rs_mx_
    CaptureTiming capture_after_;  // guarded by rs_mx_; frames from capture_until_ms_ on
    BlurQuadsFn blur_quads_;       // guarded by rs_mx_
    mutable bool blur_warned_ = false;
    insv::SeamDepthTracker depth_;                    // depth-aware seam, per frame
    std::unique_ptr<insv::SeamCosts> seam_costs_;     // its cost volume (scratch)
    bool seam_warned_ = false;
    double seam_ms_ = 0.0; // the last paramsWithSeam(), for ReadTiming::seamMs
    ClipInfo info_;
    int eq_w_ = 0;
    int eq_h_ = 0;
    bool flat_ten_bit_ = false; // SetFlatTenBit
    std::vector<ViewPane> panes_; // SetViewPanes
    PanePoseFn pane_pose_;
    uint32_t pane_fill_ = 0;
    std::string last_error_;
    ReadTiming last_timing_;
};

// The clip's lens profile for a lens-guard choice; null for none (bare lens)
// and when the file doesn't carry it.
const insv::LensProfile* LensGuardProfile(const camera::ClipMetadata& clip,
                                          PreviewStitchConfig::LensGuard guard);

// The calibration of a dual-fisheye clip (its two lenses in one file or one
// each; the decoders open them from the lenses' sources) and `params` set up
// for the lens guard. False with `err` set for any other clip. `*guardFound`
// (if given) is false when the file has no usable profile for the guard and
// the stitch takes the lens as bare.
bool DualFisheyeStitchSetup(const camera::ClipMetadata& clip,
                            PreviewStitchConfig::LensGuard guard, insv::InsvCalibration& calib,
                            insv::StitchParams& params, std::string& err,
                            bool* guardFound = nullptr);

// Returns a fresh, un-Open()'d stitch source (always a NativeStitchSource).
// Kept as a factory so a second backend could slot back in without touching
// callers.
std::unique_ptr<IStitchSource> MakeStitchSource();

} // namespace stitch
