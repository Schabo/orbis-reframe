#pragma once

// The one FFmpeg Vulkan hw device of the process. The lens decoders, the GPU
// stitch that adopts their device, and a *_vulkan export encoder all run on
// it: one VkDevice instead of one per user, and one set of FFmpeg's queue
// locks, which every submission on the device has to go through.

#include <cstdint>

struct AVBufferRef;

namespace core {

// A new reference to the device (caller unrefs), created on first use and
// kept for the process lifetime; nullptr when it can't be created (FFmpeg
// built without Vulkan, or no driver). Thread-safe.
AVBufferRef* sharedVulkanDevice();

// The graphics memory of the shared device's GPU, from VK_EXT_memory_budget,
// over its device-local heaps: `budget` is what this process can use now --
// the driver takes off what the desktop and other programs hold, so it
// moves -- and `usage` what this process uses (every device it has on that
// GPU, the viewport's too). Without the extension (or the device) `known`
// is false and the rest 0, said once in the log.
struct GpuMemory {
    bool known = false;
    uint64_t budget = 0;
    uint64_t usage = 0;
    uint64_t headroom() const { return budget > usage ? budget - usage : 0; }
};
// Cheap (a driver query, nothing allocated); thread-safe. Doesn't create the
// device: unknown until something has.
GpuMemory sharedDeviceMemory();

} // namespace core
