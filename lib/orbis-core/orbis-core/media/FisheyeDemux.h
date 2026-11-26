// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <orbis-core/Image.h>
#include <orbis-core/media/LensDemuxer.h>

#include <cstdint>
#include <string>
#include <vector>

// Random-access and sequential decode of an Insta360 `.insv` file's fisheye
// video streams.
//
// An `.insv` carries one HEVC video stream per lens (two on the X5, index 0 and
// 1, each 3840x3840) plus audio. This opens the container and decodes fisheye
// frames -- either a single frame at an arbitrary timestamp (readFrameAt, for a
// still / spike / scrub) or the stream played strictly forward (seekTo +
// readNextPair, for NativeStitchSource driving live playback).
//
// The demux, decoders and seeking are LensDemuxer's, shared with
// VulkanVideoDecoder; this adds CPU decode set-up and the RGB conversion.
// FFmpeg-dependent, hence in its own static lib (`orbiscore_media`); the
// projection/gyro core (`insv`) stays dependency-free. Cleanroom otherwise --
// no Qt, no Vulkan.
//
// One FisheyeDemux instance holds a single shared demux read position plus a
// persistent decoder per lens. Do not interleave readFrameAt() with the
// sequential API on the same instance -- readFrameAt() re-seeks and resets the
// sequential playhead. NativeStitchSource uses only the sequential API;
// tools/orbis_stitch uses only readFrameAt().

struct AVFrame;
struct SwsContext;

namespace orbiscore {

class FisheyeDemux {
public:
    FisheyeDemux() = default;
    ~FisheyeDemux();
    FisheyeDemux(const FisheyeDemux&) = delete;
    FisheyeDemux& operator=(const FisheyeDemux&) = delete;

    // Opens `path`, inventories its video streams, and spins up a persistent
    // decoder per lens. Returns false with `err` set on failure.
    bool open(const std::string& path, std::string& err);
    // The same for lenses that may come from several files (LensDemuxer::open).
    bool open(const std::vector<LensSource>& lenses, std::string& err);
    // A clip in parts, played back to back (LensDemuxer::open).
    bool open(const std::vector<ClipPart>& parts, std::string& err);

    int lensCount() const { return m_demux.lensCount(); }
    const InsvStreamInfo& lens(int i) const { return m_demux.lens(i); }
    // Lens `i`'s picture, as a read gives it: its part of the stream's frame
    // (LensSource::rect), the whole frame for a lens with a stream of its
    // own. Smaller than lens(i)'s size where lenses share a frame (a photo
    // series, a proxy).
    int lensWidth(int i) const;
    int lensHeight(int i) const;

    // Decodes lens `lens`'s frame at ~`timestampMs` into `out` (RGB24): seekTo
    // + decode forward to the first frame at or after the timestamp. Moves the
    // sequential playhead. Returns false with `err` set on
    // failure.
    bool readFrameAt(int lens, int64_t timestampMs, Image& out, std::string& err);

    // Positions the shared playhead so the next readNextPair() returns the first
    // fully-decoded frame pair at or after `timestampMs`. A target a little
    // ahead, with no keyframe since the last pair read, decodes forward from
    // there instead of from the keyframe again (LensDemuxer). Returns false
    // with `err` set on failure.
    bool seekTo(int64_t timestampMs, std::string& err);
    // See LensDemuxer::seekToUs.
    bool seekToUs(int64_t fromUs, std::string& err);

    // The keyframe nearest `timestampMs` (lens 0's index), in ms as a read
    // reports its frame (MsOfUs), so seekTo() of it lands on that keyframe
    // and decodes nothing else; `timestampMs` itself if the index is
    // unavailable. For scrubbing.
    int64_t nearestKeyframeMs(int64_t timestampMs) const;
    // See orbiscore::KeyframesAroundMs.
    void keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const;

    // Returns the next lens-0 / lens-1 fisheye frame pair (RGB24) in play order,
    // each lens its part of its frame (LensSource::rect: a camera's proxy holds
    // both in one),
    // decoding strictly forward -- no per-call seek, no decoder flush.
    // `*pts` (may be null) gets the pair's presentation time.
    // Returns false with `err` empty at end-of-stream, or `err` set on error.
    // A pair that fails to convert is an error and is gone: the next call
    // returns the pair after it. Requires exactly 2 lens streams.
    bool readNextPair(Image& out0, Image& out1, FramePts* pts, std::string& err);

private:
    void close();
    // `frm` as RGB24 into `out`; false with `err` set if swscale can't.
    bool convert(const AVFrame* frm, const InsvStreamInfo& stream, Image& out, std::string& err);
    // Cuts lens `lens`'s part (m_rect) out of `img`; a whole frame stays.
    void crop(int lens, Image& img) const;
    // Lens `lens`'s part of `from` into `out` (`from` itself: a whole frame).
    void cropInto(int lens, const Image& from, Image& out) const;

    LensDemuxer m_demux;
    Image m_frame; // both lenses' frame converted, kept for the next (readNextPair)
    SwsContext* m_sws = nullptr;
    int m_swsSrcFullRange = -1; // last range fed to sws_setColorspaceDetails (-1 = unset)
    int m_swsCoefId = -1;       // last SWS_CS_* matrix id set
    LensRect m_rect[2];         // each lens's part of its frame, the first part's
};

} // namespace orbiscore
