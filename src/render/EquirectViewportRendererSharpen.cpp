// The viewport's sharpen: the canvas a clip with a sharpen draws into, the
// compute pass that sharpens it (render::VulkanSharpen) and the slots its
// pictures are drawn from. See EquirectViewportRenderer.h.

#include "EquirectViewportRenderer.h"

#include <QVulkanWindow>

#include <cstdio>
#include <limits>

namespace {

// The canvas: what the views write in their canvas mode -- colour and label,
// in half floats, as the sharpen's canvas buffer holds them.
constexpr VkFormat kCanvasFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Result slots: the view, and this many sharpened insets a frame (a
// transition between two flat clips with a sharpen has two).
constexpr int kMaxSharpenResults = 4;

} // namespace

void EquirectViewportRenderer::sharpenUnavailable(const char *why)
{
    if (m_sharpenSaid)
        return;
    m_sharpenSaid = true;
    std::fprintf(stderr, "[viewport] %s; clips are shown without their sharpen (the export "
                         "has it)\n",
                 why);
}

void EquirectViewportRenderer::createSharpenResources()
{
    if (!m_effects.ready() || !m_luts.ready())
        return; // no effects at all, said by initResources
    VkDevice device = m_window->device();
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_window->physicalDevice(), &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(m_window->physicalDevice(), &families, props.data());
    const uint32_t family = m_window->graphicsQueueFamilyIndex();
    if (family >= families || !(props[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        sharpenUnavailable("the graphics queue can't run compute shaders");
        return;
    }
    std::string err;
    if (!m_sharpen.init({m_window->physicalDevice(), device}, m_luts.layout(), m_effects.layout(),
                        err)) {
        sharpenUnavailable(err.c_str());
        return;
    }

    VkAttachmentDescription att{};
    att.format = kCanvasFormat;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; // copied out next
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // In: the last frame's copy out of the canvas is done before it's
    // cleared. Out: the drawing is done before the copy.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    if (vkCreateRenderPass(device, &rpi, nullptr, &m_canvasPass) != VK_SUCCESS) {
        m_canvasPass = VK_NULL_HANDLE;
        m_sharpen.destroy();
        sharpenUnavailable("the canvas render pass failed");
    }
}

void EquirectViewportRenderer::destroySharpenResources()
{
    VkDevice device = m_window->device();
    for (auto &[n, slot] : m_sharpenResults) {
        destroyInsetSlot(slot);
        if (slot.set)
            vkFreeDescriptorSets(device, slot.pool, 1, &slot.set);
    }
    m_sharpenResults.clear();
    if (m_canvas.fb)
        vkDestroyFramebuffer(device, m_canvas.fb, nullptr);
    if (m_canvas.view)
        vkDestroyImageView(device, m_canvas.view, nullptr);
    if (m_canvas.img)
        vkDestroyImage(device, m_canvas.img, nullptr);
    if (m_canvas.mem)
        vkFreeMemory(device, m_canvas.mem, nullptr);
    m_canvas = {};
    for (int v = 0; v < 4; ++v) {
        if (m_canvasPipelines[v])
            vkDestroyPipeline(device, m_canvasPipelines[v], nullptr);
        m_canvasPipelines[v] = VK_NULL_HANDLE;
        m_canvasPipelineFailed[v] = false;
    }
    if (m_insetCanvasPipeline)
        vkDestroyPipeline(device, m_insetCanvasPipeline, nullptr);
    m_insetCanvasPipeline = VK_NULL_HANDLE;
    m_insetCanvasPipelineFailed = false;
    if (m_canvasPass)
        vkDestroyRenderPass(device, m_canvasPass, nullptr);
    m_canvasPass = VK_NULL_HANDLE;
    m_sharpen.destroy();
}

bool EquirectViewportRenderer::ensureCanvas(uint32_t w, uint32_t h)
{
    if (!m_sharpen.ready() || !m_canvasPass)
        return false;
    VkDevice device = m_window->device();
    const bool sized = m_canvas.img && m_canvas.w == w && m_canvas.h == h;
    if (sized && !m_sharpen.changes(int(w), int(h), VK_NULL_HANDLE))
        return true;
    if (m_canvas.failedW == w && m_canvas.failedH == h)
        return false; // tried at this size already
    // A new size (the window resized): nothing in flight may still use the
    // old canvas or the sharpen's buffers.
    vkDeviceWaitIdle(device);
    std::string err;
    bool ok = m_sharpen.ensure(int(w), int(h), VK_NULL_HANDLE, err);
    if (ok && !sized) {
        if (m_canvas.fb)
            vkDestroyFramebuffer(device, m_canvas.fb, nullptr);
        if (m_canvas.view)
            vkDestroyImageView(device, m_canvas.view, nullptr);
        if (m_canvas.img)
            vkDestroyImage(device, m_canvas.img, nullptr);
        if (m_canvas.mem)
            vkFreeMemory(device, m_canvas.mem, nullptr);
        m_canvas = {};
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = kCanvasFormat;
        ii.extent = {w, h, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ok = vkCreateImage(device, &ii, nullptr, &m_canvas.img) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device, m_canvas.img, &req);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = m_window->deviceLocalMemoryIndex();
            ok = vkAllocateMemory(device, &ai, nullptr, &m_canvas.mem) == VK_SUCCESS &&
                 vkBindImageMemory(device, m_canvas.img, m_canvas.mem, 0) == VK_SUCCESS;
        }
        if (ok) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = m_canvas.img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = kCanvasFormat;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ok = vkCreateImageView(device, &vi, nullptr, &m_canvas.view) == VK_SUCCESS;
        }
        if (ok) {
            VkFramebufferCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = m_canvasPass;
            fi.attachmentCount = 1;
            fi.pAttachments = &m_canvas.view;
            fi.width = w;
            fi.height = h;
            fi.layers = 1;
            ok = vkCreateFramebuffer(device, &fi, nullptr, &m_canvas.fb) == VK_SUCCESS;
        }
        m_canvas.w = w;
        m_canvas.h = h;
    }
    if (!ok) {
        std::fprintf(stderr, "[viewport] no graphics memory for a %ux%u sharpen canvas (%s); "
                             "the clip is shown without its sharpen\n",
                     w, h, err.empty() ? "the canvas image" : err.c_str());
        m_canvas.failedW = w;
        m_canvas.failedH = h;
        m_canvas.w = m_canvas.h = 0;
        return false;
    }
    m_canvas.failedW = m_canvas.failedH = 0;
    return true;
}

EquirectViewportRenderer::InsetSlot *EquirectViewportRenderer::sharpenResult(int n, uint32_t w,
                                                                             uint32_t h)
{
    if (n >= kMaxSharpenResults)
        return nullptr;
    auto it = m_sharpenResults.find(n);
    if (it == m_sharpenResults.end()) {
        // A set from the inset pools, as an inset's, under a key no clip
        // has (-1 is the Media tab's preview), then taken out of theirs.
        const InsetSlotKey key{std::numeric_limits<int>::min() + n, 0};
        InsetSlot *made = insetSlot(key);
        if (!made)
            return nullptr;
        it = m_sharpenResults.emplace(n, std::move(*made)).first;
        m_insetSlots.erase(key);
    }
    InsetSlot &slot = it->second;
    slot.lastUsed = m_frameCounter;
    if (!ensureInsetSlot(slot, w, h, /*staging=*/false))
        return nullptr;
    return &slot;
}

VkPipeline EquirectViewportRenderer::canvasPipeline(bool lens, bool shaped)
{
    const int v = int(lens) | int(shaped) << 1;
    if (!m_canvasPipelines[v] && !m_canvasPipelineFailed[v] && m_canvasPass) {
        namespace sh = render::shaders;
        const render::shaders::Spirv frag =
            lens ? (shaped ? sh::fisheyeViewFragShapeFx() : sh::fisheyeViewFragFx())
                 : (shaped ? sh::equirectFragShapeFx() : sh::equirectFragFx());
        m_canvasPipelines[v] = makePipeline(frag, lens ? m_lensPipelineLayout : m_pipelineLayout,
                                            false, m_canvasPass);
        m_canvasPipelineFailed[v] = m_canvasPipelines[v] == VK_NULL_HANDLE;
        if (m_canvasPipelineFailed[v])
            sharpenUnavailable("a view's canvas build failed");
    }
    return m_canvasPipelines[v];
}

VkPipeline EquirectViewportRenderer::insetCanvasPipeline()
{
    if (!m_insetCanvasPipeline && !m_insetCanvasPipelineFailed && m_canvasPass &&
        m_insetPipelineLayout) {
        m_insetCanvasPipeline = makePipeline(render::shaders::insetFragFx(),
                                             m_insetPipelineLayout, false, m_canvasPass);
        m_insetCanvasPipelineFailed = m_insetCanvasPipeline == VK_NULL_HANDLE;
        if (m_insetCanvasPipelineFailed)
            sharpenUnavailable("the inset's canvas build failed");
    }
    return m_insetCanvasPipeline;
}

void EquirectViewportRenderer::recordSharpened(VkCommandBuffer cmd, InsetSlot &result,
                                               const fx::Render &e, VkDescriptorSet lookSet,
                                               int frameW, int frameH, bool labelAlpha)
{
    const uint32_t w = m_canvas.w, h = m_canvas.h;
    // The canvas image (the render pass left it TRANSFER_SRC) into the
    // sharpen's canvas buffer -- the same texels -- once the last sharpen
    // is done reading it.
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 0, nullptr);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, m_canvas.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_sharpen.canvas(), 1, &region);
    m_sharpen.recordSharpen(cmd, int(w), int(h), render::VulkanSharpen::Format::Rgba, 1.0f, e,
                            m_effects, lookSet, frameW, frameH, labelAlpha);

    // The sharpened frame into the result's image, for the window's pass.
    VkBufferMemoryBarrier out{};
    out.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    out.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    out.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    out.srcQueueFamilyIndex = out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out.buffer = m_sharpen.out();
    out.size = VK_WHOLE_SIZE;
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = result.undefined ? VK_IMAGE_LAYOUT_UNDEFINED
                                       : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = result.img;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = result.undefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &out, 1, &toDst);
    vkCmdCopyBufferToImage(cmd, m_sharpen.out(), result.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toRead);
    result.undefined = false;
}
