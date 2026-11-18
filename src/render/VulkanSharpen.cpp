#include "VulkanSharpen.h"

#include "EmbeddedShaders.h"
#include "Sharpen.h"
#include "VulkanEffects.h"

#include <cstdint>

namespace render {

namespace {

// sharpen.comp's push constants.
struct Push {
    int32_t pass;
    int32_t w;
    int32_t h;
    int32_t fmt;
    float amount;
    float sigma;
    float threshold;
    int32_t taps;
    float white;
    uint32_t fill;
    int32_t labelAlpha;
};
static_assert(sizeof(Push) == 44, "Push must match sharpen.comp");

uint32_t groups(int n, int size) { return uint32_t((n + size - 1) / size); }

} // namespace

bool VulkanSharpen::init(const Device& dev, VkDescriptorSetLayout lutLayout,
                         VkDescriptorSetLayout effectsLayout, std::string& err)
{
    destroy();
    m_dev = dev;

    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &m_layout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout (sharpen) failed";
        destroy();
        return false;
    }
    // As the view pipelines' layouts: set 1 (the grade's LUT) isn't read
    // here, it's only there so the effects are set 2 and the look set 3.
    const VkDescriptorSetLayout sets[4] = {m_layout, lutLayout, effectsLayout, lutLayout};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 4;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pli, nullptr, &m_pipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout (sharpen) failed";
        destroy();
        return false;
    }

    const shaders::Spirv spv = shaders::sharpenComp();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spv.bytes;
    mi.pCode = spv.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &module) != VK_SUCCESS) {
        err = "vkCreateShaderModule (sharpen) failed";
        destroy();
        return false;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = m_pipeLayout;
    const VkResult made =
        vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev.device, module, nullptr);
    if (made != VK_SUCCESS) {
        m_pipeline = VK_NULL_HANDLE;
        err = "vkCreateComputePipelines (sharpen) failed";
        destroy();
        return false;
    }

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &m_layout;
    if (vkCreateDescriptorPool(dev.device, &pi, nullptr, &m_pool) != VK_SUCCESS ||
        (dai.descriptorPool = m_pool,
         vkAllocateDescriptorSets(dev.device, &dai, &m_set) != VK_SUCCESS)) {
        m_set = VK_NULL_HANDLE;
        err = "the sharpen descriptor set failed";
        destroy();
        return false;
    }
    return true;
}

void VulkanSharpen::destroy()
{
    if (m_dev.device == VK_NULL_HANDLE)
        return;
    free(m_canvas);
    free(m_across);
    free(m_out);
    m_outside = VK_NULL_HANDLE;
    if (m_pool)
        vkDestroyDescriptorPool(m_dev.device, m_pool, nullptr); // frees m_set
    if (m_pipeline)
        vkDestroyPipeline(m_dev.device, m_pipeline, nullptr);
    if (m_pipeLayout)
        vkDestroyPipelineLayout(m_dev.device, m_pipeLayout, nullptr);
    if (m_layout)
        vkDestroyDescriptorSetLayout(m_dev.device, m_layout, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
    m_pipeLayout = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_dev = {};
}

bool VulkanSharpen::make(Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage,
                         std::string& err)
{
    free(b);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_dev.device, &bi, nullptr, &b.buf) != VK_SUCCESS) {
        b.buf = VK_NULL_HANDLE;
        err = "vkCreateBuffer (sharpen) failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_dev.device, b.buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_dev.phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(m_dev.device, &ai, nullptr, &b.mem) != VK_SUCCESS ||
        vkBindBufferMemory(m_dev.device, b.buf, b.mem, 0) != VK_SUCCESS) {
        b.mem = VK_NULL_HANDLE;
        free(b);
        err = "no graphics memory for the sharpen's canvas";
        return false;
    }
    b.bytes = bytes;
    return true;
}

void VulkanSharpen::free(Buffer& b)
{
    if (b.buf)
        vkDestroyBuffer(m_dev.device, b.buf, nullptr);
    if (b.mem)
        vkFreeMemory(m_dev.device, b.mem, nullptr);
    b = {};
}

bool VulkanSharpen::changes(int w, int h, VkBuffer out) const
{
    const VkDeviceSize pixels = VkDeviceSize(w) * VkDeviceSize(h);
    return !m_canvas.buf || m_canvas.bytes < pixels * 8 || m_across.bytes < pixels * 4 ||
           (out ? out != m_outside : m_out.bytes < pixels * 4);
}

bool VulkanSharpen::ensure(int w, int h, VkBuffer out, std::string& err)
{
    if (!ready() || w <= 0 || h <= 0) {
        err = "the sharpen pipeline isn't there";
        return false;
    }
    if (!changes(w, h, out))
        return true;
    const VkDeviceSize pixels = VkDeviceSize(w) * VkDeviceSize(h);
    if (m_canvas.bytes < pixels * 8 &&
        !make(m_canvas, pixels * 8,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, err))
        return false;
    if (m_across.bytes < pixels * 4 &&
        !make(m_across, pixels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, err))
        return false;
    if (out) {
        free(m_out);
    } else if (m_out.bytes < pixels * 4 &&
               !make(m_out, pixels * 4,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, err)) {
        return false;
    }
    m_outside = out;
    writeSet();
    return true;
}

void VulkanSharpen::writeSet()
{
    VkDescriptorBufferInfo info[3] = {{m_canvas.buf, 0, VK_WHOLE_SIZE},
                                      {m_across.buf, 0, VK_WHOLE_SIZE},
                                      {out(), 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = m_set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &info[i];
    }
    vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
}

void VulkanSharpen::recordFill(VkCommandBuffer cmd, int w, int h, uint32_t rgba, float white,
                               VulkanEffectsRing& ring, VkDescriptorSet lookSet)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 0, 1, &m_set, 0,
                            nullptr);
    const float frame[4] = {0.0f, 0.0f, float(w), float(h)};
    ring.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 2, fx::Render{}, frame, lookSet);
    Push p{};
    p.pass = 0;
    p.w = w;
    p.h = h;
    p.white = white;
    p.fill = rgba;
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, groups(w, 16), groups(h, 16), 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void VulkanSharpen::recordSharpen(VkCommandBuffer cmd, int w, int h, Format fmt, float white,
                                  const fx::Render& e, VulkanEffectsRing& ring,
                                  VkDescriptorSet lookSet, int frameW, int frameH,
                                  bool labelAlpha)
{
    if (frameW <= 0 || frameH <= 0) {
        frameW = w;
        frameH = h;
    }
    // The canvas as the views left it (a shader's writes or a copy); and
    // the blur's buffer and out() free of the last frame's use.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 0, 1, &m_set, 0,
                            nullptr);
    // The per-pixel effects against the frame, from the canvas's top left.
    const float frame[4] = {0.0f, 0.0f, float(frameW), float(frameH)};
    ring.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 2, e, frame, lookSet);

    const fx::SharpenKernel k = fx::sharpenKernel(e, float(frameH));
    Push p{};
    p.w = w;
    p.h = h;
    p.fmt = int32_t(fmt);
    p.amount = e.sharpen[0];
    p.sigma = k.sigma > 0.0f ? k.sigma : 1.0f;
    p.threshold = e.sharpen[2] * white;
    p.taps = k.taps;
    p.white = white;
    p.labelAlpha = labelAlpha ? 1 : 0;
    p.pass = 1;
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, groups(w, 16), groups(h, 16), 1);

    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    p.pass = 2;
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, groups(w / 4, 16), groups(h / 2, 16), 1);
}

} // namespace render
