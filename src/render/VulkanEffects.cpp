#include "VulkanEffects.h"

#include <algorithm>
#include <cstring>

namespace render {

bool VulkanEffectsRing::init(const Device& dev, VkShaderStageFlags stages, std::string& err)
{
    destroy();
    m_dev = dev;

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b.descriptorCount = 1;
    b.stageFlags = stages;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &m_layout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout (effects) failed";
        destroy();
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev.phys, &props);
    const VkDeviceSize align = std::max<VkDeviceSize>(
        1, props.limits.minUniformBufferOffsetAlignment);
    m_stride = (sizeof(Block) + align - 1) / align * align;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = m_stride * kSlots;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &m_buf) != VK_SUCCESS) {
        err = "vkCreateBuffer (effects) failed";
        destroy();
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device, m_buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(dev.phys, &mp);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    void* map = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(dev.device, &ai, nullptr, &m_mem) != VK_SUCCESS ||
        vkBindBufferMemory(dev.device, m_buf, m_mem, 0) != VK_SUCCESS ||
        vkMapMemory(dev.device, m_mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        err = "no host-visible memory for the effects buffer";
        destroy();
        return false;
    }
    m_map = static_cast<uint8_t*>(map);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1};
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
        err = "the effects descriptor set failed";
        m_set = VK_NULL_HANDLE;
        destroy();
        return false;
    }
    VkDescriptorBufferInfo dbi{m_buf, 0, sizeof(Block)};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev.device, 1, &w, 0, nullptr);
    return true;
}

void VulkanEffectsRing::destroy()
{
    if (m_dev.device == VK_NULL_HANDLE)
        return;
    if (m_pool)
        vkDestroyDescriptorPool(m_dev.device, m_pool, nullptr); // frees m_set
    if (m_buf)
        vkDestroyBuffer(m_dev.device, m_buf, nullptr);
    if (m_mem)
        vkFreeMemory(m_dev.device, m_mem, nullptr); // unmaps
    if (m_layout)
        vkDestroyDescriptorSetLayout(m_dev.device, m_layout, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_buf = VK_NULL_HANDLE;
    m_mem = VK_NULL_HANDLE;
    m_map = nullptr;
    m_layout = VK_NULL_HANDLE;
    m_dev = {};
}

void VulkanEffectsRing::bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint,
                             VkPipelineLayout pipelineLayout, uint32_t set,
                             const fx::Render& effects, const float frame[4],
                             VkDescriptorSet lookSet)
{
    const uint32_t slot = m_next.fetch_add(1, std::memory_order_relaxed) % kSlots;
    const uint32_t offset = uint32_t(slot * m_stride);
    Block b{};
    std::memcpy(b.frame, frame, sizeof b.frame);
    std::memcpy(b.vignette, effects.vignette.data(), sizeof b.vignette);
    std::memcpy(b.grain, effects.grain.data(), sizeof b.grain);
    std::memcpy(b.look, effects.look.data(), sizeof b.look);
    std::memcpy(m_map + offset, &b, sizeof b);
    const VkDescriptorSet sets[2] = {m_set, lookSet};
    vkCmdBindDescriptorSets(cmd, bindPoint, pipelineLayout, set, 2, sets, 1, &offset);
}

} // namespace render
