#include "VulkanHwDevice.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstring>
#include <mutex>
#include <vector>

namespace core {

namespace {

std::mutex g_mx;
AVBufferRef* g_device = nullptr; // guarded by g_mx

// Whether `phys` has VK_EXT_memory_budget. The query below only needs the
// extension supported, not enabled on the device (a physical-device query).
bool HasMemoryBudget(VkPhysicalDevice phys)
{
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> ext(n);
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, ext.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties& e : ext)
        if (std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0)
            return true;
    return false;
}

} // namespace

AVBufferRef* sharedVulkanDevice()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_device &&
        av_hwdevice_ctx_create(&g_device, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) < 0)
        g_device = nullptr;
    return g_device ? av_buffer_ref(g_device) : nullptr;
}

GpuMemory sharedDeviceMemory()
{
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_device) {
            const auto* dctx = reinterpret_cast<const AVHWDeviceContext*>(g_device->data);
            phys = static_cast<const AVVulkanDeviceContext*>(dctx->hwctx)->phys_dev;
        }
    }
    if (!phys)
        return {};
    // The device is kept for the process lifetime, so its GPU is too.
    static const bool hasBudget = [phys] {
        const bool has = HasMemoryBudget(phys);
        if (!has)
            std::fprintf(stderr, "[gpu memory] the driver doesn't report the graphics memory "
                                 "budget (VK_EXT_memory_budget); keeping only the clips in "
                                 "use on the graphics card\n");
        return has;
    }();
    if (!hasBudget)
        return {};
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb{};
    mb.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 mp{};
    mp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    mp.pNext = &mb;
    vkGetPhysicalDeviceMemoryProperties2(phys, &mp);
    GpuMemory m;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; ++i) {
        if (!(mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            continue;
        m.budget += mb.heapBudget[i];
        m.usage += mb.heapUsage[i];
    }
    m.known = m.budget > 0;
    return m;
}

uint64_t gpuMargin(const GpuMemory& m)
{
    return std::max<uint64_t>(uint64_t(512) << 20, m.budget / 10);
}

namespace {
std::atomic<uint64_t> g_clipBytes{0};
}

void setClipGpuBytes(uint64_t bytes)
{
    g_clipBytes.store(bytes);
}

uint64_t clipGpuBytes()
{
    return g_clipBytes.load();
}

namespace {
std::atomic<int> g_jobs{0};
}

GpuJobScope::GpuJobScope()
{
    g_jobs.fetch_add(1);
}

GpuJobScope::~GpuJobScope()
{
    g_jobs.fetch_sub(1);
}

bool gpuJobRunning()
{
    return g_jobs.load() > 0;
}

bool waitForGpuRoom(int maxMs, const std::atomic<bool>* cancel)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    for (;;) {
        const GpuMemory m = sharedDeviceMemory();
        if (!m.known || m.headroom() >= gpuMargin(m) + clipGpuBytes())
            return true;
        if ((cancel && cancel->load()) || std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace core
