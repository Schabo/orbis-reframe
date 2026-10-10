#pragma once

// privacy::FrameSource on a stitch::NativeStitchSource: the plate scan's
// views rendered straight from the fisheyes on the GPU (one held frame,
// NativeStitchSource::HoldNextPair / RenderHeldViews), or -- without the
// fused view path (CPU decode / stitch) -- cut on the CPU from the stitched
// equirect. Levelled with the clip's gyro in Horizon lock.

#include "PlateScanner.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace insv {
class GyroStabilizer;
}
namespace stitch {
class NativeStitchSource;
}

namespace privacy {

class StitchFrameSource : public FrameSource {
public:
    // `src`: opened, configured like playback, and seeked to `fromMs`. Frames
    // in [fromMs, toMs] (source time). `level`: Horizon lock for the clip;
    // null or not ready = no levelling.
    StitchFrameSource(stitch::NativeStitchSource& src, int64_t fromMs, int64_t toMs, double fps,
                      std::shared_ptr<const insv::GyroStabilizer> level);

    bool next(int64_t& tMs, float level[9], std::string& err) override;
    bool render(int canvasW, int canvasH, const std::vector<ScanView>& views, std::vector<uint8_t>& rgba,
                std::string& err) override;
    void release() override;

    // Whether views come from the GPU (the fused path) or the CPU fallback.
    bool gpu() const { return m_gpu; }

private:
    stitch::NativeStitchSource& m_src;
    int64_t m_fromMs, m_toMs;
    double m_halfFrameMs;
    std::shared_ptr<const insv::GyroStabilizer> m_level;
    bool m_gpu = false;
    float m_M[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    // CPU fallback: the current frame's equirect.
    std::vector<uint8_t> m_eq;
    int m_eqW = 0, m_eqH = 0;
};

// The same, backwards: frames from `startMs` down to `fromMs` (source time),
// newest first -- following a plate back from where the user boxed it.
// HEVC only decodes forwards (keyframes every ~2 s on the X5), so it decodes
// a stretch of up to `chunkFrames` forward (SeekTo + KeepNextPair, kept on
// the GPU), hands those out from the end, then the stretch before. Each
// stretch's seek decodes from its keyframe again: ~1.5-2x the decoding of a
// forward pass. The depth-aware seam is measured stretch by stretch in play
// order, so it may differ a little from playback's. GPU (fused view) path
// only: on the CPU fallback next() fails with an error.
class ReverseStitchFrameSource : public FrameSource {
public:
    // `src`: opened and configured like playback (its position doesn't
    // matter). chunkFrames <= 0: as many as ~1 GB of GPU memory holds, 8-60.
    ReverseStitchFrameSource(stitch::NativeStitchSource& src, int64_t fromMs, int64_t startMs, double fps,
                             std::shared_ptr<const insv::GyroStabilizer> level, int chunkFrames = 0);
    ~ReverseStitchFrameSource() override;

    bool next(int64_t& tMs, float level[9], std::string& err) override;
    bool render(int canvasW, int canvasH, const std::vector<ScanView>& views, std::vector<uint8_t>& rgba,
                std::string& err) override;
    void release() override;

private:
    struct Kept {
        int handle = 0;
        int64_t tMs = 0;
    };
    bool loadChunk(std::string& err); // the stretch before m_boundary
    void releaseAll();

    stitch::NativeStitchSource& m_src;
    int64_t m_fromMs;
    double m_frameMs;
    double m_boundary; // frames still to come are before this (ms)
    std::shared_ptr<const insv::GyroStabilizer> m_level;
    int m_chunk;
    bool m_done = false;
    std::deque<Kept> m_kept; // the current stretch, time order; handed out from the back
    int m_current = 0;       // the frame handed out last (released on next() / release())
    float m_M[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

} // namespace privacy
