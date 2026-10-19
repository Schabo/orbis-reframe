#include "VulkanHwDevice.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

#include <mutex>

namespace core {

AVBufferRef* sharedVulkanDevice()
{
    static std::mutex mx;
    static AVBufferRef* device = nullptr;
    std::lock_guard<std::mutex> lk(mx);
    if (!device && av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) < 0)
        device = nullptr;
    return device ? av_buffer_ref(device) : nullptr;
}

} // namespace core
