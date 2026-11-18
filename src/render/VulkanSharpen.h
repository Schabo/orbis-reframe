#pragma once

#include "ClipEffects.h" // fx::Render

#include <vulkan/vulkan.h>

#include <string>

namespace render {

class VulkanEffectsRing;

// The sharpen effect on the GPU (shaders/sharpen.comp; src/core/Sharpen.h is
// its CPU twin), for the GPU export (VulkanFisheyeStitcher) and the viewport
// (EquirectViewportRenderer), each on its own device.
//
// A clip with a sharpen isn't drawn straight into its frame: its views draw
// the canvas -- the graded picture with its look, and each pixel's label
// (clip_effects.glsl's `canvas`) -- then this blurs it across and down, only
// within a label, sharpens, runs the vignette and grain and writes the frame
// out (NV12, RGBA8 or P010, the view shaders' layouts).
class VulkanSharpen {
public:
    struct Device {
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
    };
    // sharpen.comp's `fmt`.
    enum class Format { Nv12 = 0, Rgba = 1, P010 = 2 };

    VulkanSharpen() = default;
    VulkanSharpen(const VulkanSharpen&) = delete;
    VulkanSharpen& operator=(const VulkanSharpen&) = delete;
    ~VulkanSharpen() { destroy(); }

    // The pipeline. `lutLayout` and `effectsLayout`: the device's
    // VulkanLutStore and VulkanEffectsRing set layouts (sets 1 / 3 and 2 of
    // the pipeline layout, as the view pipelines'), both made with the
    // compute stage.
    bool init(const Device& dev, VkDescriptorSetLayout lutLayout,
              VkDescriptorSetLayout effectsLayout, std::string& err);
    // Free everything; the caller makes sure the GPU is done with it.
    void destroy();
    bool ready() const { return m_pipeline != VK_NULL_HANDLE; }

    // Whether ensure(w, h, out) would replace a buffer or point the set
    // somewhere else: the caller then waits for the GPU to be done with the
    // old ones first.
    bool changes(int w, int h, VkBuffer out) const;
    // The canvas and the blur's buffer for a w x h frame (they only grow),
    // and `out`, where the frame is written (VK_NULL_HANDLE: a buffer of
    // its own, RGBA8, with transfer-source use -- out() -- for the
    // viewport). The canvas has transfer-destination use, so it can be
    // copied into. False (err set) when there's no memory.
    bool ensure(int w, int h, VkBuffer out, std::string& err);
    // The canvas: w x h pixels, 8 bytes each (half-float red, green, blue,
    // label -- an RGBA16F image's texels).
    VkBuffer canvas() const { return m_canvas.buf; }
    VkBuffer out() const { return m_out.buf ? m_out.buf : m_outside; }

    // Pass 0: the canvas filled with `rgba` (R in the low byte) and label 0,
    // in canvas units of `white`; then a barrier, so the views can draw.
    // `ring` and `lookSet` as recordSharpen's (the shader reads sets 2 and 3
    // in its other passes, so they're bound for this one too).
    void recordFill(VkCommandBuffer cmd, int w, int h, uint32_t rgba, float white,
                    VulkanEffectsRing& ring, VkDescriptorSet lookSet);
    // After the views drew the canvas (by shader or transfer writes; the
    // barrier is here): passes 1 and 2 -- the sharpen of `e`, its other
    // per-pixel effects (bound from `ring` as set 2, `lookSet` set 3: any
    // valid LUT set), the frame written to out() as `fmt`. `white`: the
    // canvas's white (255 for the export's views, 1 for the viewport's).
    // The caller adds the barrier for whatever reads out() next.
    // frameW x frameH: the frame from the canvas's top left, when it's
    // smaller than the canvas (0 = the canvas): the vignette and grain are
    // placed against it, the radius against its height. `labelAlpha`
    // (RGBA8): pixels without a picture transparent, not opaque.
    void recordSharpen(VkCommandBuffer cmd, int w, int h, Format fmt, float white,
                       const fx::Render& e, VulkanEffectsRing& ring, VkDescriptorSet lookSet,
                       int frameW = 0, int frameH = 0, bool labelAlpha = false);

private:
    struct Buffer {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
    };
    bool make(Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string& err);
    void free(Buffer& b);
    void writeSet();

    Device m_dev;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    Buffer m_canvas, m_across, m_out;
    VkBuffer m_outside = VK_NULL_HANDLE; // a caller's out buffer
};

} // namespace render
