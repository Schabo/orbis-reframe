// The GPU colour LUT store (render::VulkanLutStore) on a real Vulkan device:
// a frame that binds more LUTs than kMaxLuts keeps every set it was handed,
// the store sheds the extra ones once a frame no longer uses them, and
// without beginFrame() it stays at kMaxLuts as before.
// Standalone; the exit code is the failure count, 77 (skipped) without a
// Vulkan device.

#include "VulkanLut.h"

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;

    // The first device with a graphics queue; false (and nothing made) if
    // there's none.
    bool open()
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &app;
        if (vkCreateInstance(&ii, nullptr, &instance) != VK_SUCCESS)
            return false;
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());
        for (VkPhysicalDevice d : devs) {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
            for (uint32_t q = 0; q < qn; ++q) {
                if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                    continue;
                const float prio = 1.0f;
                VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                qi.queueFamilyIndex = q;
                qi.queueCount = 1;
                qi.pQueuePriorities = &prio;
                VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                di.queueCreateInfoCount = 1;
                di.pQueueCreateInfos = &qi;
                if (vkCreateDevice(d, &di, nullptr, &device) != VK_SUCCESS)
                    continue;
                phys = d;
                family = q;
                vkGetDeviceQueue(device, q, 0, &queue);
                return true;
            }
        }
        return false;
    }

    ~Gpu()
    {
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};

// A 2^3 LUT, different for every `i`.
std::shared_ptr<const color::Lut3D> lut(int i)
{
    auto l = std::make_shared<color::Lut3D>();
    l->size = 2;
    for (int b = 0; b < 2; ++b)
        for (int g = 0; g < 2; ++g)
            for (int r = 0; r < 2; ++r)
                l->rgb.insert(l->rgb.end(), {r * 0.5f + i * 0.01f, float(g), float(b)});
    return l;
}

bool initStore(render::VulkanLutStore &store, const Gpu &gpu)
{
    render::VulkanLutStore::Device dev;
    dev.phys = gpu.phys;
    dev.device = gpu.device;
    dev.queue = gpu.queue;
    dev.queueFamily = gpu.family;
    dev.waitIdle = [d = gpu.device] { vkDeviceWaitIdle(d); };
    std::string err;
    if (!store.init(dev, VK_SHADER_STAGE_FRAGMENT_BIT, err)) {
        std::printf("FAIL  init: %s\n", err.c_str());
        ++g_failures;
        return false;
    }
    return true;
}

constexpr size_t kMax = render::VulkanLutStore::kMaxLuts;

// More LUTs in one frame than the limit: every set handed out stays valid
// (distinct, none freed under the frame), and the store holds them all.
void testFrameKeepsItsLuts(const Gpu &gpu)
{
    render::VulkanLutStore store;
    if (!initStore(store, gpu))
        return;
    std::vector<std::shared_ptr<const color::Lut3D>> luts;
    for (size_t i = 0; i < kMax + 4; ++i)
        luts.push_back(lut(int(i)));

    store.beginFrame();
    std::set<VkDescriptorSet> sets;
    for (const auto &l : luts) {
        std::string err;
        const VkDescriptorSet s = store.setFor(l, &err);
        CHECK(err.empty());
        CHECK(s != VK_NULL_HANDLE);
        sets.insert(s);
    }
    CHECK(sets.size() == luts.size());
    CHECK(store.uploaded() == luts.size());

    // The same LUTs again: the same sets, no new uploads.
    store.beginFrame();
    CHECK(store.uploaded() == luts.size()); // all used in the last frame
    std::set<VkDescriptorSet> again;
    for (const auto &l : luts)
        again.insert(store.setFor(l));
    CHECK(again == sets);
    CHECK(store.uploaded() == luts.size());

    // A frame with only two of them: the next one sheds back to the limit,
    // keeping the two.
    store.beginFrame();
    const VkDescriptorSet a = store.setFor(luts[0]);
    const VkDescriptorSet b = store.setFor(luts[1]);
    store.beginFrame();
    CHECK(store.uploaded() == kMax);
    CHECK(store.setFor(luts[0]) == a);
    CHECK(store.setFor(luts[1]) == b);
    CHECK(store.uploaded() == kMax);
}

// Without beginFrame() (the GPU stitch: one LUT per submission) the store
// never holds more than kMaxLuts.
void testWithoutFrames(const Gpu &gpu)
{
    render::VulkanLutStore store;
    if (!initStore(store, gpu))
        return;
    std::vector<std::shared_ptr<const color::Lut3D>> luts;
    for (size_t i = 0; i < kMax + 4; ++i) {
        luts.push_back(lut(int(i)));
        CHECK(store.setFor(luts.back()) != VK_NULL_HANDLE);
        CHECK(store.uploaded() <= kMax);
    }
    CHECK(store.uploaded() == kMax);
}

} // namespace

int main()
{
    Gpu gpu;
    if (!gpu.open()) {
        std::printf("vulkan_lut_test: no Vulkan device, skipped\n");
        return 77;
    }
    testFrameKeepsItsLuts(gpu);
    testWithoutFrames(gpu);
    if (g_failures == 0)
        std::printf("vulkan_lut_test: all checks passed\n");
    return g_failures;
}
