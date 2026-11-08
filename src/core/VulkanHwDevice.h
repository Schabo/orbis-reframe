#pragma once

// The one FFmpeg Vulkan hw device of the process. The lens decoders, the GPU
// stitch that adopts their device, and a *_vulkan export encoder all run on
// it: one VkDevice instead of one per user, and one set of FFmpeg's queue
// locks, which every submission on the device has to go through.

#include <atomic>
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

// The headroom to keep free on the graphics card beside the clips: for the
// viewport's swapchain when the window is resized (a rebuild that runs out
// leaves the viewport off until a restart), the desktop, and an encoder. A
// tenth of the budget, at least 512 MiB.
uint64_t gpuMargin(const GpuMemory& m);

// What one clip's decode and stitch take on the graphics card, as playback
// measured its sources when they opened and let go (the largest clip's),
// 0 before any. What a measurement or a job about to open a clip plans
// for. Thread-safe.
void setClipGpuBytes(uint64_t bytes);
uint64_t clipGpuBytes();

// For a job about to open a clip on the GPU, on its own thread, after
// asking playback and the background measurements to make room: waits
// (polling) until the card has room for one more clip beside the margin,
// for at most `maxMs` or until `*cancel`. Returns at once when it has, or
// when there's no budget to tell. Letting go takes them a moment, and a
// job that opens meanwhile finds the memory still taken. True when there's
// room (or no telling).
bool waitForGpuRoom(int maxMs, const std::atomic<bool>* cancel = nullptr);

// A job on the GPU (an export, a still, a subject track, a plate scan) runs
// while one of these lives, on its thread: work that could wait and would
// take graphics memory -- the background measurements, playback trying a
// clip that fell back to the CPU on the GPU again -- waits for it to end.
class GpuJobScope {
public:
    GpuJobScope();
    ~GpuJobScope();
    GpuJobScope(const GpuJobScope&) = delete;
    GpuJobScope& operator=(const GpuJobScope&) = delete;
};
bool gpuJobRunning(); // thread-safe

} // namespace core
