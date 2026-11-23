// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <orbis-core/media/LensDemuxer.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// GPU-resident sequential decode of an .insv file's two fisheye HEVC streams
// via FFmpeg's Vulkan hwaccel. Frames never leave VRAM: readNextPair() hands
// back two AVFrame*s carrying AVVkFrame (NV12, one 2-plane image each), which a
// Vulkan compute stitch samples directly.
//
// The decoder runs on an FFmpeg Vulkan hw device the caller creates
// (av_hwdevice_ctx_create with AV_HWDEVICE_TYPE_VULKAN) and passes to open();
// FFmpeg owns its VkInstance/VkDevice, and deviceHandles() exposes them so the
// compute stitch can run on the same device. A program with several decoders
// is best served by one device for all of them: creating one costs ~0.3 s,
// and on the NVIDIA driver it stalls other threads' presents meanwhile, so
// opening a clip mid-playback shouldn't create another. When decoders on
// several threads share it, a submission to it from outside FFmpeg must hold
// the queue's lock (queueLock()).
//
// Frames are allocated CONCURRENT across FFmpeg's queue families, so no
// queue-family ownership transfer is needed.
//
// The demux, decoders and seeking are LensDemuxer's, shared with FisheyeDemux;
// this adds the Vulkan hwaccel set-up and the frame locking.
//
// Part of orbiscore_media (needs FFmpeg + Vulkan). Not thread-safe.

struct AVBufferRef;
struct AVFrame;

namespace orbiscore {

using VulkanVideoStreamInfo = InsvStreamInfo;

// One decoded fisheye frame's Vulkan handles, pulled from its AVVkFrame under
// the frame lock (see lockFrame). Consumed by render::VulkanFisheyeStitcher.
struct VulkanLensFrame {
    VkImage image = VK_NULL_HANDLE;   // 2-plane: G8_B8R8_2PLANE_420_UNORM (NV12), or
                                      // G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 (P010) for 10-bit
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags2 access = 0;              // the image's last access, as FFmpeg recorded it
    VkSemaphore semaphore = VK_NULL_HANDLE; // timeline
    uint64_t semWaitValue = 0;              // wait on this before reading
    int width = 0;
    int height = 0;
    VkImageUsageFlags usage = 0; // the frames pool's (plane copies need TRANSFER_SRC)
};

// What the consumer left the image as, written back into the AVVkFrame by
// unlockFrame so FFmpeg's next use of it syncs correctly. A consumer that
// did no GPU work on it hands back the VulkanLensFrame's own three.
struct VulkanLensFrameResult {
    uint64_t semSignalValue = 0; // the value the consumer signalled
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags2 access = 0;   // its last access in newLayout
};

class VulkanVideoDecoder {
public:
    VulkanVideoDecoder() = default;
    ~VulkanVideoDecoder();
    VulkanVideoDecoder(const VulkanVideoDecoder&) = delete;
    VulkanVideoDecoder& operator=(const VulkanVideoDecoder&) = delete;

    // Creates the per-lens decoders on `hwDevice` (an FFmpeg Vulkan hw
    // device; the decoder takes its own reference, the caller keeps its) and
    // decodes each lens's first frame. Returns false with `err` set if
    // `hwDevice` is null, or Vulkan video decode is unavailable or doesn't
    // start (caller falls back).
    bool open(AVBufferRef* hwDevice, const std::string& path, std::string& err);
    // The same for lenses that may come from several files (LensDemuxer::open).
    bool open(AVBufferRef* hwDevice, const std::vector<LensSource>& lenses, std::string& err);
    // A clip in parts, played back to back (LensDemuxer::open).
    bool open(AVBufferRef* hwDevice, const std::vector<ClipPart>& parts, std::string& err);
    // After a failed open(): true when the GPU can't decode this video at all
    // (no decode queue for the codec), false when it should have and the
    // decoder or its device failed to start (most often out of graphics
    // memory).
    bool unsupported() const { return m_unsupported; }
    // After a failed open() that was unsupported(): the codec's name
    // ("H.264") when the GPU decodes video, just not this codec; empty when
    // it has no video decode at all.
    const std::string& unsupportedCodec() const { return m_unsupportedCodec; }

    int lensCount() const { return m_demux.lensCount(); }
    const VulkanVideoStreamInfo& lens(int i) const { return m_demux.lens(i); }
    int64_t pairPictureBytes() const { return m_demux.pairPictureBytes(); }

    // FFmpeg's Vulkan device, for the compute stitch to adopt. `queueFlags`
    // are the flags FFmpeg created its queues with
    // (AVVulkanDeviceContext::queue_flags, FFmpeg 9+: INTERNALLY_SYNCHRONIZED
    // when the driver has VK_KHR_internally_synchronized_queues, which makes
    // vkGetDeviceQueue return NULL; retrieve with vkGetDeviceQueue2). 0 on
    // older FFmpeg. Valid after open().
    void deviceHandles(VkInstance& inst, VkPhysicalDevice& phys, VkDevice& dev,
                       uint32_t& computeQueueFamily, VkDeviceQueueCreateFlags& queueFlags) const;

    // The VkFormat of the images the decoders actually produce, read from the
    // first frame of each lens (decoded by open(), still handed out by the
    // next readNextPair): G8_B8R8_2PLANE_420_UNORM for 8-bit, G10X6..._3PACK16 for
    // Main 10, whatever the driver chose otherwise. VK_FORMAT_UNDEFINED if no
    // frame decodes or the two lenses differ.
    VkFormat frameFormat();

    // Frame-exact: the next readNextPair returns the frame at `timestampMs`.
    // A target a little ahead, with no keyframe since the last pair read,
    // decodes forward from there instead of from the keyframe again.
    bool seekTo(int64_t timestampMs, std::string& err);
    // See LensDemuxer::seekToUs.
    bool seekToUs(int64_t fromUs, std::string& err);
    // See FisheyeDemux::nearestKeyframeMs.
    int64_t nearestKeyframeMs(int64_t timestampMs) const;
    // See orbiscore::KeyframesAroundMs.
    void keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const;

    // Next lens-0 / lens-1 frame pair in play order (AVFrame* carrying
    // AVVkFrame). The caller owns them -- av_frame_free() both when done, after
    // unlockFrame(). Returns false with `err` empty at EOS, `err` set on error.
    bool readNextPair(AVFrame*& out0, AVFrame*& out1, FramePts* pts, std::string& err);

    // Take the frame lock and read its current Vulkan state into `lf`. Must be
    // paired with unlockFrame(). Call just before command submission.
    void lockFrame(AVFrame* f, VulkanLensFrame& lf);
    // A reference to the Vulkan hw device that keeps it (and its VkDevice)
    // alive past this decoder -- for GPU objects created on the device that
    // may outlive it (lens ring slots still shown by the viewport).
    std::shared_ptr<void> deviceKeepAlive() const;
    // FFmpeg's lock on queue (`family`, `index`) of the shared device -- the
    // one it takes around its own submissions. Hold it around any
    // vkQueueSubmit / vkQueueWaitIdle on that queue. The functions keep the
    // device alive. Valid after open().
    struct QueueLock {
        std::function<void()> lock, unlock;
    };
    QueueLock queueLock(uint32_t family, uint32_t index) const;
    // Write the post-submission state back and release the frame lock.
    void unlockFrame(AVFrame* f, const VulkanLensFrameResult& r);
    // The same for a pair from readNextPair. Both lenses of one stream share
    // a frame: it is locked once, both get its state, and r0 is written back.
    void lockPair(AVFrame* f0, AVFrame* f1, VulkanLensFrame& lf0, VulkanLensFrame& lf1);
    void unlockPair(AVFrame* f0, AVFrame* f1, const VulkanLensFrameResult& r0,
                    const VulkanLensFrameResult& r1);
    // av_frame_free() wrapper so callers need no FFmpeg headers.
    void freeFrame(AVFrame*& f);

private:
    void close();
    // Shared by the open()s: the device, then `openDemux` with the hwaccel
    // decoder set-up.
    bool open(AVBufferRef* hwDevice,
              const std::function<bool(const LensDemuxer::DecoderSetup&, std::string&)>& openDemux,
              std::string& err);

    LensDemuxer m_demux;
    AVBufferRef* m_hwDevice = nullptr;
    bool m_unsupported = false;
    std::string m_unsupportedCodec;
};

} // namespace orbiscore
