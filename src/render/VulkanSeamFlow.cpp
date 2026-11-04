// VulkanFisheyeStitcher's seam flow (seam_flow.comp): the flow between the
// lenses' strips and the window over the last frames, on the GPU, recorded
// into the submission that measured the strips -- nothing comes back to the
// CPU. Mirrors insv::SeamFlowEstimator (OpenCV's DIS, fast preset) and
// insv::SeamFlowWindow.
#include "VulkanFisheyeStitcherImpl.h"

#include "EmbeddedShaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>

namespace render {

namespace {

// seam_flow.comp's passes and push constants.
enum SeamFlowPass : int32_t {
    kPyramid = 0,
    kGradient = 1,
    kInit = 2,
    kSweepFwd = 3,
    kSweepBwd = 4,
    kDensify = 5,
    kStore = 6,
    kAverage = 7,
};

struct SeamFlowPush {
    int32_t pass;
    int32_t cols, rows;
    int32_t W, H;
    int32_t pw, ph;
    int32_t npx, npy;
    int32_t f;
    int32_t img, grad, patches, dense, prevDense;
    int32_t iters;
    int32_t slot, count;
    float radX, radY, sx;
};
static_assert(sizeof(SeamFlowPush) == 84, "SeamFlowPush must match seam_flow.comp's push constants");
static_assert(insv::kSeamFlowFrames == 5, "seam_flow.comp's RING is the window's length");

// DIS at OpenCV's fast preset (what the CPU estimator runs): the finest
// scale a quarter of the strip, 8 x 8 patches every 4 pixels (the shader's
// PATCH / STRIDE), 16 descent iterations per patch and scale, half in each
// propagation sweep.
constexpr int kFinestScale = 2;
constexpr int kPatch = 8;
constexpr int kStride = 4;
constexpr int kDescentIters = 8;
constexpr uint32_t kSweepStretch = 20; // seam_flow.comp's SEG_LEN
constexpr uint32_t kGroup = 64; // seam_flow.comp's local size

uint32_t Groups(int n) { return (static_cast<uint32_t>(std::max(n, 1)) + kGroup - 1) / kGroup; }

} // namespace

void VulkanFisheyeStitcher::Impl::destroySeamFlow()
{
    auto killBuf = [&](VkBuffer& b, VkDeviceMemory& m) {
        if (b) vkDestroyBuffer(device, b, nullptr);
        if (m) vkFreeMemory(device, m, nullptr);
        b = VK_NULL_HANDLE;
        m = VK_NULL_HANDLE;
    };
    killBuf(sf.work, sf.workMem);
    killBuf(sf.ring, sf.ringMem);
    sf.cols = sf.rows = 0;
    sf.scales.clear();
    sf.slot = sf.count = 0;
}

void VulkanFisheyeStitcher::Impl::destroySeamFlowPipeline()
{
    destroySeamFlow();
    if (sf.pipeline) vkDestroyPipeline(device, sf.pipeline, nullptr);
    if (sf.layout) vkDestroyPipelineLayout(device, sf.layout, nullptr);
    if (sf.pool) vkDestroyDescriptorPool(device, sf.pool, nullptr);
    if (sf.setLayout) vkDestroyDescriptorSetLayout(device, sf.setLayout, nullptr);
    sf.pipeline = VK_NULL_HANDLE;
    sf.layout = VK_NULL_HANDLE;
    sf.pool = VK_NULL_HANDLE;
    sf.setLayout = VK_NULL_HANDLE;
    sf.set = VK_NULL_HANDLE;
}

bool VulkanFisheyeStitcher::Impl::buildSeamFlowPipeline(std::string& err)
{
    if (sf.pipeline)
        return true;
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 3; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 4;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(device, &sli, nullptr, &sf.setLayout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout (seam flow) failed";
        return false;
    }
    const VkDescriptorPoolSize ps[2]{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(device, &dpi, nullptr, &sf.pool) != VK_SUCCESS) {
        err = "vkCreateDescriptorPool (seam flow) failed";
        return false;
    }
    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = sf.pool;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &sf.setLayout;
    if (vkAllocateDescriptorSets(device, &dsi, &sf.set) != VK_SUCCESS) {
        err = "vkAllocateDescriptorSets (seam flow) failed";
        return false;
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SeamFlowPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &sf.setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device, &pli, nullptr, &sf.layout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout (seam flow) failed";
        return false;
    }
    const shaders::Spirv spv = shaders::seamFlowComp();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.bytes;
    smi.pCode = spv.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smi, nullptr, &module) != VK_SUCCESS) {
        err = "vkCreateShaderModule (seam flow) failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = sf.layout;
    const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &sf.pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (r != VK_SUCCESS) {
        err = "vkCreateComputePipelines (seam flow) failed";
        return false;
    }
    return true;
}

bool VulkanFisheyeStitcher::Impl::ensureSeamFlow(const insv::StitchGpuParams& p, std::string& err)
{
    const int cols = p.seamFlowCols, rows = p.seamFlowRows;
    if (cols <= 0 || rows <= 1 || (cols & 3) || !(p.seamFlowStripRad > 0)) {
        err = "no seam flow grid in the params";
        return false;
    }
    // The texture at the grid's size, whatever field the frame had before.
    insv::StitchGpuParams grid;
    grid.seamFlowCols = cols;
    grid.seamFlowRows = rows;
    grid.seamFlowGpu = true;
    if (!buildSeamFlowPipeline(err) ||
        !ensureStrips(static_cast<VkDeviceSize>(cols) * rows * 2, err) || !ensureFlow(grid, err))
        return false;
    if (flowW != cols || flowH != rows) {
        err = "seam flow texture isn't the grid's size";
        return false;
    }
    sf.radX = static_cast<float>(2.0 * std::numbers::pi / cols);
    sf.radY = static_cast<float>(-2.0 * p.seamFlowStripRad / (rows - 1));
    sf.reachRad = p.seamFlowReachRad;
    if (cols != sf.cols || rows != sf.rows) {
        idleBeforeFree();
        destroySeamFlow();
        // The pyramid as OpenCV's DIS picks it: down to where the strip's
        // height still holds a patch or two (its width allows far more).
        const int coarsest = std::max(
            kFinestScale,
            std::min(static_cast<int>(std::log2(cols / (4.0 * kPatch)) + 0.5),
                     static_cast<int>(std::log2(rows / static_cast<double>(kPatch)))));
        int at = 0; // floats
        for (int s = kFinestScale; s <= coarsest; ++s) {
            SeamFlowScale sc;
            sc.f = 1 << s;
            sc.W = std::max(kPatch, static_cast<int>(std::lround(cols / static_cast<double>(sc.f))));
            sc.H = (rows + sc.f - 1) / sc.f;
            sc.sx = static_cast<float>(cols / static_cast<double>(sc.W));
            sc.npx = (sc.W + kStride - 1) / kStride;
            sc.npy = sc.H <= kPatch ? 1 : (sc.H - kPatch + kStride - 1) / kStride + 1;
            const int px = sc.W * sc.H;
            sc.img = at;
            at += 2 * px;
            sc.grad = at;
            at += 4 * px;
            sc.patches = at;
            at += 4 * sc.npx * sc.npy;
            sc.dense = at;
            at += 4 * px;
            sf.scales.push_back(sc);
        }
        const SeamFlowScale& fin = sf.scales.front();
        if (!makeBuffer(static_cast<VkDeviceSize>(at) * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sf.work, sf.workMem, nullptr) ||
            !makeBuffer(static_cast<VkDeviceSize>(insv::kSeamFlowFrames) * fin.W * fin.H * 16,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        sf.ring, sf.ringMem, nullptr)) {
            destroySeamFlow();
            err = "seam flow buffer allocation failed";
            return false;
        }
        sf.cols = cols;
        sf.rows = rows;
    }
    // The strip buffer and the texture may have been made anew: point the
    // set at what's there now (every submission before has been waited on).
    VkDescriptorBufferInfo bi[3]{{stripBuf, 0, VK_WHOLE_SIZE},
                                 {sf.work, 0, VK_WHOLE_SIZE},
                                 {sf.ring, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, flowView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = sf.set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        if (i < 3) {
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        } else {
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i].pImageInfo = &ii;
        }
    }
    vkUpdateDescriptorSets(device, 4, w, 0, nullptr);
    return true;
}

void VulkanFisheyeStitcher::Impl::recordSeamFlow(VkCommandBuffer c)
{
    auto barrier = [&] {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    auto push = [&](const SeamFlowScale& sc, const SeamFlowScale* coarser) {
        SeamFlowPush p{};
        p.cols = sf.cols;
        p.rows = sf.rows;
        p.W = sc.W;
        p.H = sc.H;
        p.pw = coarser ? coarser->W : 0;
        p.ph = coarser ? coarser->H : 0;
        p.npx = sc.npx;
        p.npy = sc.npy;
        p.f = sc.f;
        p.img = sc.img;
        p.grad = sc.grad;
        p.patches = sc.patches;
        p.dense = sc.dense;
        p.prevDense = coarser ? coarser->dense : 0;
        p.iters = kDescentIters;
        p.slot = sf.slot;
        p.count = std::min(sf.count + 1, insv::kSeamFlowFrames);
        p.radX = sf.radX;
        p.radY = sf.radY;
        p.sx = sc.sx;
        return p;
    };
    auto run = [&](SeamFlowPush p, int32_t pass, uint32_t gx, uint32_t gy, uint32_t gz) {
        p.pass = pass;
        vkCmdPushConstants(c, sf.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(c, gx, gy, gz);
    };

    barrier(); // the strips, written by the strip pass or copied in
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, sf.pipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, sf.layout, 0, 1, &sf.set, 0, nullptr);
    for (const SeamFlowScale& sc : sf.scales)
        run(push(sc, nullptr), kPyramid, Groups(sc.W * sc.H), 1, 2);
    barrier();
    for (const SeamFlowScale& sc : sf.scales)
        run(push(sc, nullptr), kGradient, Groups(sc.W * sc.H), 1, 2);
    barrier();
    const int n = static_cast<int>(sf.scales.size());
    for (int s = n - 1; s >= 0; --s) {
        const SeamFlowScale& sc = sf.scales[s];
        const SeamFlowPush p = push(sc, s + 1 < n ? &sf.scales[s + 1] : nullptr);
        const uint32_t patches = Groups(sc.npx * sc.npy);
        run(p, kInit, patches, 1, 2);
        barrier();
        const uint32_t segs = (static_cast<uint32_t>(sc.npx) + kSweepStretch - 1) / kSweepStretch;
        run(p, kSweepFwd, segs, static_cast<uint32_t>(sc.npy), 2);
        barrier();
        run(p, kSweepBwd, segs, static_cast<uint32_t>(sc.npy), 2);
        barrier();
        run(p, kDensify, Groups(sc.W * sc.H), 1, 2);
        barrier();
    }
    const SeamFlowScale& fin = sf.scales.front();
    run(push(fin, nullptr), kStore, Groups(fin.W * fin.H), 1, 1);
    barrier();

    // The window's mean into the texture the stitch samples.
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.oldLayout = flowFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = flowImg;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &ib);
    run(push(fin, nullptr), kAverage, Groups(sf.cols * sf.rows), 1, 1);
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &ib);
    flowFresh = false;
    flowHeld.reset(); // the texture is the GPU's field now, not a CPU one
    sf.slot = (sf.slot + 1) % insv::kSeamFlowFrames;
    sf.count = std::min(sf.count + 1, insv::kSeamFlowFrames);
}

void VulkanFisheyeStitcher::resetSeamFlow()
{
    d->sf.slot = 0;
    d->sf.count = 0;
}

int VulkanFisheyeStitcher::seamFlowFrames() const { return d->sf.count; }

namespace {
float FromHalf(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const int exp = (h >> 10) & 0x1f;
    const uint32_t mant = h & 0x3ffu;
    float f;
    if (exp == 0) {
        f = std::ldexp(static_cast<float>(mant), -24);
    } else if (exp == 31) {
        f = mant ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    } else {
        f = std::ldexp(static_cast<float>(mant | 0x400u), exp - 25);
    }
    uint32_t x;
    std::memcpy(&x, &f, 4);
    x |= sign;
    std::memcpy(&f, &x, 4);
    return f;
}
} // namespace

bool VulkanFisheyeStitcher::Impl::submitOnce(const std::function<void(VkCommandBuffer)>& rec,
                                             std::string& err)
{
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    rec(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fence);
    return true;
}

bool VulkanFisheyeStitcher::estimateSeamFlow(const insv::SeamFlowStrips& s, std::string& err)
{
    const insv::SeamFlowGrid& g = s.grid;
    const size_t n = static_cast<size_t>(g.rows) * g.cols;
    if (!m_ready || !g.valid() || s.grey[0].size() != n || s.grey[1].size() != n) {
        err = "estimateSeamFlow: not ready or invalid strips";
        return false;
    }
    insv::StitchGpuParams p;
    p.seamFlowCols = g.cols;
    p.seamFlowRows = g.rows;
    p.seamFlowStripRad = g.stripRad;
    p.seamFlowReachRad = g.reachRad;
    p.seamFlowGpu = true;
    if (!d->ensureSeamFlow(p, err) || !d->ensureFlow(p, err))
        return false;
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    void* map = nullptr;
    const VkDeviceSize bytes = 2 * n;
    if (!d->makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stage,
                       stageMem, &map)) {
        if (stage) vkDestroyBuffer(d->device, stage, nullptr);
        err = "estimateSeamFlow: staging buffer failed";
        return false;
    }
    std::memcpy(map, s.grey[0].data(), n);
    std::memcpy(static_cast<uint8_t*>(map) + n, s.grey[1].data(), n);
    const bool ok = d->submitOnce([&](VkCommandBuffer c) {
        d->recordFlowUpload(c);
        const VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(c, stage, d->stripBuf, 1, &region);
        d->recordSeamFlow(c);
    }, err);
    vkUnmapMemory(d->device, stageMem);
    vkDestroyBuffer(d->device, stage, nullptr);
    vkFreeMemory(d->device, stageMem, nullptr);
    return ok;
}

bool VulkanFisheyeStitcher::readSeamFlow(insv::SeamFlowField& out, std::string& err)
{
    out.v.clear();
    if (!m_ready || !d->flowImg || d->sf.cols != d->flowW || d->sf.rows != d->flowH) {
        err = "readSeamFlow: no GPU seam flow";
        return false;
    }
    const int W = d->flowW, H = d->flowH;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(W) * H * 8;
    if (!d->makeReadbackBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, buf, mem, &map)) {
        if (buf) vkDestroyBuffer(d->device, buf, nullptr);
        err = "readSeamFlow: readback buffer failed";
        return false;
    }
    const bool ok = d->submitOnce([&](VkCommandBuffer c) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = 0; // every submission before was waited on
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = d->flowImg;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {static_cast<uint32_t>(W), static_cast<uint32_t>(H), 1};
        vkCmdCopyImageToBuffer(c, d->flowImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
        VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.buffer = buf;
        hb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                             1, &hb, 0, nullptr);
    }, err);
    if (ok) {
        out.grid.cols = W;
        out.grid.rows = H;
        out.grid.stripRad = static_cast<float>(-d->sf.radY * (H - 1) / 2.0);
        out.grid.reachRad = d->sf.reachRad;
        out.v.resize(static_cast<size_t>(W) * H * 4);
        const uint16_t* h = static_cast<const uint16_t*>(map);
        for (size_t k = 0; k < out.v.size(); ++k)
            out.v[k] = FromHalf(h[k]);
    }
    vkUnmapMemory(d->device, mem);
    vkDestroyBuffer(d->device, buf, nullptr);
    vkFreeMemory(d->device, mem, nullptr);
    return ok;
}

} // namespace render
