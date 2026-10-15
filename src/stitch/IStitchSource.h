#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace render {
struct LensFrame; // src/render/LensFrame.h
}
namespace insv {
struct BlurQuad; // src/insv/BlurQuads.h
}

namespace stitch {

// Privacy blur: the regions to mosaic in the frame at source time ptsMs
// (insv::BlurQuad, equirect / sphere frame), appended to `out`. Called on the
// reading thread for every frame stitched.
using BlurQuadsFn = std::function<void(int64_t ptsMs, std::vector<insv::BlurQuad>& out)>;

// Camera rotation from video time tSec to tSec + dtSec as a rotation vector
// (rad, sphere frame at tSec) -- insv::GyroStabilizer::bodyRotation.
using RsRotationFn = std::function<void(double tSec, double dtSec, float r[3])>;

// When each part of a frame was actually captured, relative to the frame time
// the stabilization is computed for, so the stitch can de-rotate every lens /
// sensor row to that one instant (the camera keeps turning in between; at the
// stick mount's ~11 Hz shake a few ms is a visible wobble).
//   lensOffsetSec: capture time of lens 0 / lens 1's picture (mid-sensor
//     row), relative to the frame time.
//   readoutSec: per lens, rows read top to bottom over this time around the
//     lens's capture time (0 = all rows at once).
// GyroStabilizer::lensCaptureOffsetsSec / lensReadoutSec give the X5's.
struct CaptureTiming {
    double lensOffsetSec[2] = {0.0, 0.0};
    double readoutSec[2] = {0.0, 0.0};
    bool scanColumns = false;
    RsRotationFn rotationAt; // empty => off
    bool active() const
    {
        return rotationAt && (readoutSec[0] != 0.0 || readoutSec[1] != 0.0 ||
                              lensOffsetSec[0] != 0.0 || lensOffsetSec[1] != 0.0);
    }
};

// Preview-stitch knobs surfaced in the Stitching settings panel. Persisted
// per-project on the .orp (Project::previewStitch), pushed to the live stitch
// path via IStitchSource::Open.
//
// NativeStitchSource honours width/height (the equirect output size),
// lensGuard and depthSeam. `quality` is inert (no optical-flow seam yet). Stabilization is not a stitch setting: insv::StabConfig.
struct PreviewStitchConfig {
    enum class Quality { Template, OpticalFlow };

    Quality quality = Quality::Template;
    // Equirect preview resolution. Must stay 2:1 (equirect is 360x180 and the
    // reprojection shader assumes it) -- the panel only offers 2:1 presets.
    // 0 = Auto: per clip, AutoPreviewWidth() of its lens size and frame rate.
    int width = 0;
    int height = 0;

    // What's fitted over the lenses. The camera doesn't record it, so the user
    // picks it; it selects the matching lens profile in the clip's trailer
    // (docs/formats/insta360-x5.md §5.4). Guards shift the image ~2 % outward at the
    // seam -- the wrong choice leaves a ~4 deg ghost there. Defaults to
    // Premium: all the project's footage is shot with them.
    enum class LensGuard { None, Standard, Premium };
    LensGuard lensGuard = LensGuard::Premium;

    // Depth-aware seam: measure, per frame, the distance along the seam at
    // which the two lenses agree and stitch each lens from its own centre at
    // that distance, so near content (bike, rider, stick) isn't doubled. Off =
    // both lenses treated as one point (infinite stitch distance).
    bool depthSeam = true;

    // A stable per-config string (logs, tests); nothing caches by it today.
    std::string cacheTag() const {
        const std::string size =
            width > 0 ? std::to_string(width) + "x" + std::to_string(height) : "auto";
        return "v7-" + size + "-" + (quality == Quality::OpticalFlow ? "of" : "tmpl") + "-g" +
               std::to_string(static_cast<int>(lensGuard)) + (depthSeam ? "-d" : "");
    }

    bool operator==(const PreviewStitchConfig& o) const {
        return quality == o.quality && width == o.width && height == o.height &&
               lensGuard == o.lensGuard && depthSeam == o.depthSeam;
    }
    bool operator!=(const PreviewStitchConfig& o) const { return !(*this == o); }
};

// Equirect widths (height = width / 2), from an X5 clip's lens size (each lens
// is lensWidth square; the full sphere is 2 x lensWidth wide).
inline int FullSphereWidth(int lensWidth) { return lensWidth > 0 ? 2 * lensWidth : 3840; }

// Auto preview: the full sphere, capped at what still plays in real time at
// this frame rate (tools/playback_bench, RTX 5070: decode + stitch + copy-back is
// 20.7 ms at 5760 for 8K30, 14 ms at 5120 for 5.7K60, 5.4 ms at 3840 for
// 4K120). 4K120's sphere is 3840, so every mode gets as close to full source
// detail as it can afford.
inline int AutoPreviewWidth(int lensWidth, double fps) {
    const int cap = fps > 90.0 ? 3840 : fps > 45.0 ? 5120 : 5760;
    const int full = FullSphereWidth(lensWidth);
    return (full < cap ? full : cap) & ~1;
}

// Export: all the detail the clip has, and at least 2x the output width so the
// rectilinear reproject never magnifies the equirect.
inline int ExportStitchWidth(int lensWidth, int outWidth) {
    int w = FullSphereWidth(lensWidth);
    if (w < 2 * outWidth)
        w = 2 * outWidth;
    if (w < 3840)
        w = 3840;
    return w & ~1;
}

// Describes the source clip itself (native resolution/fps/duration) — not
// necessarily the resolution frames are actually served at. See Frame.
struct ClipInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t duration_ms = 0;
};

// One stitched equirect frame, decoded to interleaved 8-bit RGBA. width/
// height are this frame's actual dimensions, which for a live-playback
// source is the cached preview resolution, not ClipInfo's source resolution.
struct Frame {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    int64_t timestamp_ms = 0;
};

// Serves stitched equirect frames from an Insta360 .insv file for interactive
// playback/scrubbing.
//
// The implementation (stitch::NativeStitchSource, src/insv/) does true
// frame-at-time-T stitching in-process: Open() only parses the calibration
// trailer and opens the demux, then SeekTo()/ReadFrame()/GetFrame() decode the
// lens pair at the requested time and stitch on demand (Vulkan compute, with a
// CPU fallback). No up-front whole-file pass, no on-disk cache. Full-resolution
// export drives the same interface at a larger equirect size (see
// ReframeExportController).
class IStitchSource {
public:
    virtual ~IStitchSource() = default;

    // Opens the clip for stitching with `config` (preview size, lens guard,
    // depth seam). Returns false (with GetLastError() set) on failure.
    virtual bool Open(const std::string& clip_path, const PreviewStitchConfig& config) = 0;

    // Valid only after a successful Open().
    virtual const ClipInfo& GetInfo() const = 0;

    // Random-access single frame: the frame at timestamp_ms. Equivalent to
    // SeekTo(timestamp_ms) + ReadFrame(), and it moves the sequential playhead
    // the same way. Good for scrubbing; for continuous playback use SeekTo +
    // ReadFrame directly so the decoder runs forward without a per-frame seek.
    // Valid only after a successful Open().
    virtual Frame GetFrame(int64_t timestamp_ms) = 0;

    // Positions the sequential playhead. The next ReadFrame() returns the first
    // frame at or after timestamp_ms. Blocks until that frame is decoded and
    // stitched. Returns false with GetLastError() set on failure. Valid only
    // after a successful Open().
    virtual bool SeekTo(int64_t timestamp_ms) = 0;

    // The source time (ms) of the keyframe nearest `timestamp_ms`: SeekTo() of
    // it decodes a single frame, so it's the cheap target while scrubbing. The
    // default (no index) is `timestamp_ms` itself.
    virtual int64_t NearestKeyframeMs(int64_t timestamp_ms) const { return timestamp_ms; }
    // The keyframes at or before / at or after `timestamp_ms` (source ms, as
    // NearestKeyframeMs); -1 where there's none. The default (no index)
    // knows none.
    virtual void KeyframesAround(int64_t timestamp_ms, int64_t& before, int64_t& after) const {
        (void)timestamp_ms;
        before = after = -1;
    }

    // Returns the next frame in play order and advances the playhead,
    // decoding strictly forward -- no seek, no decoder flush. Returns an empty
    // Frame (rgba.empty()) at end-of-clip; also on error, with GetLastError()
    // set. Not safe to call concurrently with itself, GetFrame, or SeekTo.
    virtual Frame ReadFrame() = 0;

    virtual const std::string& GetLastError() const = 0;

    // Per-lens / per-sensor-row capture-time correction for subsequent frames
    // (see CaptureTiming, NativeStitchSource): `timing` for frames before
    // source time `untilMs`, `after` for frames from it on -- so the reader
    // can hand over to the next clip of the same file at a cut without
    // knowing which frame will cross it. An inactive timing turns it off.
    // Safe to call from another thread than the reader. Default:
    // unsupported, ignored.
    virtual void SetCaptureTiming(CaptureTiming /*timing*/, int64_t /*untilMs*/,
                                  CaptureTiming /*after*/) {}
    // One timing for every frame.
    void SetCaptureTiming(CaptureTiming timing)
    {
        SetCaptureTiming(std::move(timing), INT64_MAX, CaptureTiming{});
    }

    // Seam alignment fine-tune (insv::StitchParams::lens1AdjustDeg, measured
    // by stitch::MeasureClipSeam): lens 1's extra yaw / pitch / roll, degrees,
    // for subsequently stitched frames. Call from the reading thread.
    // Default: unsupported, ignored.
    virtual void SetLens1Adjust(const double /*deg*/[3]) {}

    // Privacy blur for subsequent frames: every frame stitched is mosaicked
    // inside the quads `fn` returns for its time (empty fn = off). Safe to
    // call from another thread than the reader. False when the source can't
    // blur -- a caller that must not show the footage unblurred stops there.
    virtual bool SetBlurQuads(BlurQuadsFn /*fn*/) { return false; }

    // The next frame as the decoded fisheye pair on the GPU, shareable with
    // another Vulkan device (render::LensFrame), instead of a stitched equirect
    // -- for a viewport that renders the view straight from the lenses. Same
    // play order / seeking as ReadFrame(). False at end-of-clip (GetLastError()
    // empty) or on error. Default: unsupported; LensFramesAvailable() tells.
    virtual bool LensFramesAvailable() const { return false; }
    virtual bool ReadLensFrame(render::LensFrame& /*out*/) { return false; }
    // Let go of GPU memory held for lens frames that nobody is using (the
    // source went idle). Default: nothing to do.
    virtual void TrimLensFrames() {}
};

} // namespace stitch
