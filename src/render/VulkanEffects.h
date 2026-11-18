#pragma once

#include "ClipEffects.h" // fx::Render

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace render {

// A clip's video effects for the shaders' EFFECTS builds
// (clip_effects.glsl's `fx` block, set 2, and its colour look's LUT, set 3):
// one host-visible uniform buffer,
// a ring of small blocks, and one descriptor set bound with a dynamic offset
// per draw or dispatch. Used by the viewport (EquirectViewportRenderer) and
// the GPU export (VulkanFisheyeStitcher), each on its own device.
//
// The push-constant blocks are full (Vulkan's guaranteed 128 bytes), hence a
// buffer. A block is written when it's bound and not touched again until the
// ring comes round, kSlots binds later -- far more than any caller has in
// flight (a frame binds one per pane, inset or transition layer).
class VulkanEffectsRing {
public:
    struct Device {
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
    };

    VulkanEffectsRing() = default;
    VulkanEffectsRing(const VulkanEffectsRing&) = delete;
    VulkanEffectsRing& operator=(const VulkanEffectsRing&) = delete;
    ~VulkanEffectsRing() { destroy(); }

    // The set layout, the buffer and the set. `stages`: the shader stages
    // that read it.
    bool init(const Device& dev, VkShaderStageFlags stages, std::string& err);
    // Free everything; the caller makes sure the GPU is done with it.
    void destroy();
    bool ready() const { return m_set != VK_NULL_HANDLE; }
    VkDescriptorSetLayout layout() const { return m_layout; }

    // Write `effects` and the output frame -- x, y of its top-left corner,
    // its width and height, in the shader's own pixel coordinates -- into the
    // next block, and bind it as set `set` of `pipelineLayout`, with
    // `lookSet` (the colour look's LUT, VulkanLutStore::setFor; the identity
    // one for none) as set `set` + 1. `canvasLabel` > 0: the draw writes
    // the sharpen's canvas with that label (clip_effects.glsl's `canvas`,
    // VulkanSharpen).
    void bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout pipelineLayout,
              uint32_t set, const fx::Render& effects, const float frame[4],
              VkDescriptorSet lookSet, float canvasLabel = 0.0f);

    static constexpr uint32_t kSlots = 1024;

private:
    // std140, as clip_effects.glsl's block.
    struct Block {
        float frame[4];
        float vignette[4];
        float grain[4];
        float look[4];
        float canvas[4];
    };

    Device m_dev;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkBuffer m_buf = VK_NULL_HANDLE;
    VkDeviceMemory m_mem = VK_NULL_HANDLE;
    uint8_t* m_map = nullptr;
    VkDeviceSize m_stride = 0;
    std::atomic<uint32_t> m_next{0};
};

} // namespace render
