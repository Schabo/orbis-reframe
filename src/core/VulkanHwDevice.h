#pragma once

// The one FFmpeg Vulkan hw device of the process. The lens decoders, the GPU
// stitch that adopts their device, and a *_vulkan export encoder all run on
// it: one VkDevice instead of one per user, and one set of FFmpeg's queue
// locks, which every submission on the device has to go through.

struct AVBufferRef;

namespace core {

// A new reference to the device (caller unrefs), created on first use and
// kept for the process lifetime; nullptr when it can't be created (FFmpeg
// built without Vulkan, or no driver). Thread-safe.
AVBufferRef* sharedVulkanDevice();

} // namespace core
