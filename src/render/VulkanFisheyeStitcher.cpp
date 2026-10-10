#include "VulkanFisheyeStitcher.h"

#include "EmbeddedShaders.h"

#include <SeamDepth.h> // insv::SeamCosts, the cost pass's candidates / patch

#include <vulkan/vulkan.h>

#include <unistd.h> // close() -- exported memory FDs

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

namespace render {

namespace {

constexpr uint32_t kLocalSize = 8;

double msBetween(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// std140 layout matching fisheye_stitch.comp's `Params` uniform block.
struct ShaderParams {
    float w2l0[3][4];
    float w2l1[3][4];
    float outRot[3][4];
    float cxcy[4];
    float seam[4];
    float edge[4];
    int32_t dims[4];
    float meiA[4];  // xi0, xi1, fx0, fx1
    float meiB[4];  // fy0, fy1, -, accessory
    float kd0[4];   // lens 0 radial k1..k4
    float kd1[4];   // lens 1 radial k1..k4
    float rs[4];    // capture timing: active, useColumns, -, -
    float rsRot[2 * insv::StitchGpuParams::kRsTaps][4]; // per lens, rotation vectors (stitch world frame), -
    float pd0[4];   // lens 0 tangential p1, p2, thin-prism s1, s2
    float pd1[4];   // lens 1 likewise
    float sd[4];    // thin-prism s3, s4 of lens 0, then of lens 1
    float accA[4];  // accessory poly c0..c3
    float accB[4];  // accessory poly c4, c5, bare poly c0, c1
    float accC[4];  // bare poly c2..c5
    float seamA[4]; // depth-aware seam: axis xyz, active
    float seamU[4]; // seam-plane u xyz, half baseline (m)
    float seamV[4]; // seam-plane v xyz, ramp past the band (rad)
    float seamC[4]; // cost pass: q min, q max (1/m), patch elevation half-range (rad), -
    float seamQ[insv::kSeamDepthBins]; // vec4[kSeamDepthBins / 4]: per-bin inverse distance
    float blur0[4]; // privacy blur: count, -, -, -
    float blurQ[insv::kMaxBlurQuads * 6][4]; // per quad, see fisheye_stitch_common.glsl
};
static_assert(sizeof(ShaderParams) == 7344, "ShaderParams must match the std140 UBO");
static_assert(sizeof(ShaderParams) == LensFrame::kParamsBytes, "LensFrame::params carries ShaderParams");

// Push constants for reframe_reproject.comp. 4 ints + 5 floats, then a mat3
// (16-byte aligned in std430 -> 12 bytes pad) for the stabilization matrix.
struct ReframePush {
    int32_t eqW;
    int32_t eqH;
    int32_t outW;
    int32_t outH;
    float yaw;
    float pitch;
    float roll;
    float fovY;
    float aspect;
    float _pad[3];
    float stab[12];      // 3 columns, each a vec3 padded to 16 bytes (col-major)
    float colorGrade[4]; // exposureEv, contrast%, saturation%, temperature%
};
static_assert(sizeof(ReframePush) == 112, "ReframePush must match the compute push_constant block");

// Push constants for fisheye_view_nv12.comp: 4 ints + 5 floats, pad, then the
// stab mat3 as padded columns and the grade -- the same tail as ReframePush.
struct ViewPush {
    int32_t pass;
    int32_t outW;
    int32_t outH;
    int32_t fmt; // 0 = NV12, 1 = RGBA
    float yaw;
    float pitch;
    float roll;
    float fovY;
    float aspect;
    int32_t ox;     // RGBA: the view's origin in the canvas (ox % 4 == 0) ...
    int32_t oy;
    int32_t stride; // ... and the canvas row length; 0 = outW
    float stab[12];
    float colorGrade[4];
};
static_assert(sizeof(ViewPush) == 112, "ViewPush must match the compute push_constant block");

void packParams(const insv::StitchGpuParams& p, int eqW, int eqH, ShaderParams& sp)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            sp.w2l0[r][c] = p.worldToLens[0][r * 3 + c];
            sp.w2l1[r][c] = p.worldToLens[1][r * 3 + c];
            sp.outRot[r][c] = p.outRot[r * 3 + c];
        }
        sp.w2l0[r][3] = sp.w2l1[r][3] = sp.outRot[r][3] = 0.0f;
    }
    sp.cxcy[0] = p.cx[0];
    sp.cxcy[1] = p.cy[0];
    sp.cxcy[2] = p.cx[1];
    sp.cxcy[3] = p.cy[1];
    sp.seam[0] = p.seamRad;
    sp.seam[1] = p.bandRad;
    sp.seam[2] = p.thetaMaxRad;
    sp.seam[3] = p.thetaRef;
    sp.edge[0] = p.edgeStartFrac;
    sp.edge[1] = p.edgeEndFrac;
    sp.edge[2] = static_cast<float>(p.photometric);
    sp.edge[3] = 0.0f;
    sp.dims[0] = p.fisheyeW;
    sp.dims[1] = p.fisheyeH;
    sp.dims[2] = eqW;
    sp.dims[3] = eqH;
    sp.meiA[0] = p.xi[0];
    sp.meiA[1] = p.xi[1];
    sp.meiA[2] = p.fx[0];
    sp.meiA[3] = p.fx[1];
    sp.meiB[0] = p.fy[0];
    sp.meiB[1] = p.fy[1];
    sp.meiB[2] = 0.0f;
    sp.meiB[3] = static_cast<float>(p.accessory);
    for (int k = 0; k < 4; ++k) {
        sp.kd0[k] = p.kdist[0][k];
        sp.kd1[k] = p.kdist[1][k];
    }
    float* pd[2] = {sp.pd0, sp.pd1};
    for (int i = 0; i < 2; ++i) {
        pd[i][0] = p.pdist[i][0];
        pd[i][1] = p.pdist[i][1];
        pd[i][2] = p.sdist[i][0];
        pd[i][3] = p.sdist[i][1];
        sp.sd[2 * i + 0] = p.sdist[i][2];
        sp.sd[2 * i + 1] = p.sdist[i][3];
    }
    const float acc[12] = {p.accessoryPoly[0], p.accessoryPoly[1], p.accessoryPoly[2],
                           p.accessoryPoly[3], p.accessoryPoly[4], p.accessoryPoly[5],
                           p.barePoly[0],      p.barePoly[1],      p.barePoly[2],
                           p.barePoly[3],      p.barePoly[4],      p.barePoly[5]};
    std::memcpy(sp.accA, acc, sizeof sp.accA);
    std::memcpy(sp.accB, acc + 4, sizeof sp.accB);
    std::memcpy(sp.accC, acc + 8, sizeof sp.accC);
    // The capture-time rotations arrive in the equirect frame; the shader's
    // lens rays are outRot * eqDir, so carry them into that frame.
    auto toStitchWorld = [&p](const float r[3], float out[4]) {
        for (int k = 0; k < 3; ++k)
            out[k] = p.outRot[k * 3 + 0] * r[0] + p.outRot[k * 3 + 1] * r[1] + p.outRot[k * 3 + 2] * r[2];
        out[3] = 0.0f;
    };
    constexpr int kTaps = insv::StitchGpuParams::kRsTaps;
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < kTaps; ++k)
            toStitchWorld(p.rsRot[i][k], sp.rsRot[i * kTaps + k]);
    sp.rs[0] = static_cast<float>(p.rsActive);
    sp.rs[1] = static_cast<float>(p.rsUseColumns);
    sp.rs[2] = sp.rs[3] = 0.0f;
    for (int k = 0; k < 3; ++k) {
        sp.seamA[k] = p.seamAxis[k];
        sp.seamU[k] = p.seamU[k];
        sp.seamV[k] = p.seamV[k];
    }
    sp.seamA[3] = static_cast<float>(p.depthActive);
    sp.seamU[3] = p.halfBaseline;
    sp.seamV[3] = p.depthRampRad;
    sp.seamC[0] = static_cast<float>(insv::kSeamQMin);
    sp.seamC[1] = static_cast<float>(insv::kSeamQMax);
    sp.seamC[2] = static_cast<float>(insv::kSeamPatchElDeg * std::numbers::pi / 180.0);
    sp.seamC[3] = 0.0f;
    std::memcpy(sp.seamQ, p.seamQ, sizeof sp.seamQ);
    // Privacy blur quads, sphere frame -> stitch world (rotations keep the
    // tangent-plane coordinates, so only the three axes move).
    const int nBlur = std::clamp(p.blurCount, 0, insv::kMaxBlurQuads);
    sp.blur0[0] = static_cast<float>(nBlur);
    sp.blur0[1] = sp.blur0[2] = sp.blur0[3] = 0.0f;
    std::memset(sp.blurQ, 0, sizeof sp.blurQ);
    for (int k = 0; k < nBlur; ++k) {
        const insv::BlurQuadFrame& b = p.blur[k];
        float(*q)[4] = &sp.blurQ[k * 6];
        toStitchWorld(b.c, q[0]);
        toStitchWorld(b.r, q[1]);
        toStitchWorld(b.u, q[2]);
        q[0][3] = b.cells[0];
        q[1][3] = b.cells[1];
        q[2][3] = b.cosRadius;
        q[3][0] = b.p[0][0];
        q[3][1] = b.p[0][1];
        q[3][2] = b.p[1][0];
        q[3][3] = b.p[1][1];
        q[4][0] = b.p[2][0];
        q[4][1] = b.p[2][1];
        q[4][2] = b.p[3][0];
        q[4][3] = b.p[3][1];
        q[5][0] = b.scale;
    }
}

// Interleave an RGB24 buffer into RGBA8 (opaque alpha), parallel over rows.
void expandRgbToRgba(const uint8_t* src, uint8_t* dst, int w, int h)
{
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> nextRow{0};
    auto worker = [&] {
        for (int y = nextRow.fetch_add(1); y < h; y = nextRow.fetch_add(1)) {
            const uint8_t* s = src + static_cast<size_t>(y) * w * 3;
            uint8_t* dpix = dst + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                dpix[0] = s[0];
                dpix[1] = s[1];
                dpix[2] = s[2];
                dpix[3] = 255;
                s += 3;
                dpix += 4;
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < hw; ++i)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
}

// The exportable lens ring (render::LensFrame). Shared-owned: the stitcher
// holds it, and so does every LensSlot handle given out (its deleter marks the
// slot free), so slots -- and the device they live on, via keepAlive -- outlast
// the stitcher while a viewport still shows one.
struct LensRingCore {
    struct Slot {
        LensSlot pub;
        VkImage img[LensSlot::kImages] = {};
        VkDeviceMemory mem[LensSlot::kImages] = {};
        bool busy = false;
        uint64_t fill = 0;
    };

    VkDevice device = VK_NULL_HANDLE;
    std::shared_ptr<void> keepAlive; // the adopted device's owner
    std::mutex mx;
    std::condition_variable cv;
    std::vector<std::unique_ptr<Slot>> slots; // guarded by mx

    static std::atomic<uint64_t> nextId;

    ~LensRingCore()
    {
        for (auto& sl : slots)
            destroySlot(*sl);
    }

    void destroySlot(Slot& sl)
    {
        for (int i = 0; i < LensSlot::kImages; ++i) {
            if (sl.pub.fd[i] >= 0)
                ::close(sl.pub.fd[i]);
            if (sl.img[i])
                vkDestroyImage(device, sl.img[i], nullptr);
            if (sl.mem[i])
                vkFreeMemory(device, sl.mem[i], nullptr);
            sl.pub.fd[i] = -1;
            sl.img[i] = VK_NULL_HANDLE;
            sl.mem[i] = VK_NULL_HANDLE;
        }
    }
};
std::atomic<uint64_t> LensRingCore::nextId{1};

// Ring sizing: enough for the playback queue plus what a viewport holds (the
// shown frame and its frames in flight). Past kLensSlotsSoft a fill waits for
// a slot to come back; past kLensSlotsHard it fails instead of growing.
constexpr size_t kLensSlotsSoft = 10;
constexpr size_t kLensSlotsHard = 16;
constexpr auto kLensSlotWait = std::chrono::milliseconds(250);

} // namespace

struct VulkanFisheyeStitcher::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // Timestamp queries: 0 before the stitch, 1 after it, 2 after the reproject.
    VkQueryPool tsPool = VK_NULL_HANDLE; // null if the queue has no timestamps
    double tsPeriodNs = 0.0;
    StitchTiming timing;

    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion ycbcr = VK_NULL_HANDLE; // nv12 mode only
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    bool adopted = false; // device is FFmpeg's -- don't destroy it
    // Adopted: FFmpeg's lock on `queue` -- the device is shared by every
    // decoder in the process, on other threads (setQueueLock).
    std::function<void()> lockQueue, unlockQueue;

    VkResult submit(const VkSubmitInfo& si)
    {
        if (lockQueue)
            lockQueue();
        const VkResult r = vkQueueSubmit(queue, 1, &si, fence);
        if (unlockQueue)
            unlockQueue();
        return r;
    }
    // Before freeing resources our submissions use. Each submission is waited
    // on (its fence) before the call that made it returns, so on the shared,
    // adopted device there is nothing to wait for -- and a vkDeviceWaitIdle
    // there would race other threads' queue use.
    void idleBeforeFree()
    {
        if (!adopted)
            vkDeviceWaitIdle(device);
    }
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr; // exportable ring
    uint8_t deviceUuid[16] = {};
    std::shared_ptr<void> keepAlive;
    std::shared_ptr<LensRingCore> ring;
    bool nv12 = false;    // stitchVk path: sample decoder NV12 images via ycbcr
    // Cached ycbcr image views over the decoder's pooled images (nv12 mode).
    std::map<VkImage, VkImageView> nv12Views;

    // Dimension-dependent resources.
    int fw = 0, fh = 0, ew = 0, eh = 0;
    VkImage fisheye[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory fisheyeMem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView fisheyeView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer staging = VK_NULL_HANDLE, ubo = VK_NULL_HANDLE, outBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE, uboMem = VK_NULL_HANDLE, outMem = VK_NULL_HANDLE;
    void* stagingMap = nullptr;
    void* uboMap = nullptr;
    void* outMap = nullptr;
    VkBuffer gainBuf = VK_NULL_HANDLE;      // 3 uints: photometric sum0, sum1, count
    VkDeviceMemory gainMem = VK_NULL_HANDLE;
    void* gainMap = nullptr;
    // Depth-aware seam cost volume (binding 5): (NCC, texture) per bin x candidate.
    static constexpr VkDeviceSize kSeamBufBytes =
        sizeof(float) * 2 * insv::kSeamDepthBins * insv::kSeamDepthCands;
    VkBuffer seamBuf = VK_NULL_HANDLE;
    VkDeviceMemory seamMem = VK_NULL_HANDLE;
    void* seamMap = nullptr;

    // Reproject pass (stitchAndReframeVk): equirect OutBuf -> rectilinear NV12.
    VkDescriptorSetLayout rfSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool rfDescPool = VK_NULL_HANDLE;
    VkDescriptorSet rfSet = VK_NULL_HANDLE;
    VkPipelineLayout rfPipeLayout = VK_NULL_HANDLE;
    VkPipeline rfPipeline = VK_NULL_HANDLE;
    VkBuffer flatBuf = VK_NULL_HANDLE; // host-visible flat output (NV12 or RGBA)
    VkDeviceMemory flatMem = VK_NULL_HANDLE;
    void* flatMap = nullptr;
    VkDeviceSize flatBytes = 0; // size backing flatBuf

    // Fused fisheye -> view pass (renderViewVk): the stitch's set layout, its
    // own set (binding 3 = flatBuf) and push constants.
    VkDescriptorSet vwSet = VK_NULL_HANDLE;
    VkPipelineLayout vwPipeLayout = VK_NULL_HANDLE;
    VkPipeline vwPipeline = VK_NULL_HANDLE;

    // (Re)allocate the host-visible flat output buffer to hold `bytes`. Grows
    // only, so switching NV12 <-> RGBA or sizes doesn't thrash.
    bool ensureFlat(VkDeviceSize bytes, std::string& err)
    {
        if (flatBuf && bytes <= flatBytes)
            return true;
        idleBeforeFree();
        if (flatMap) { vkUnmapMemory(device, flatMem); flatMap = nullptr; }
        if (flatBuf) vkDestroyBuffer(device, flatBuf, nullptr);
        if (flatMem) vkFreeMemory(device, flatMem, nullptr);
        flatBuf = VK_NULL_HANDLE;
        flatMem = VK_NULL_HANDLE;
        flatBytes = 0;
        if (!makeReadbackBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, flatBuf, flatMem,
                                &flatMap)) {
            err = "flat output buffer allocation failed";
            return false;
        }
        flatBytes = bytes;
        return true;
    }

    bool findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t& out) const
    {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want) {
                out = i;
                return true;
            }
        }
        return false;
    }

    // A buffer the CPU reads back every frame: host-cached if the device has
    // it. Plain HOST_VISIBLE|COHERENT system memory is write-combined, and CPU
    // reads from it run at ~0.3 GB/s (tools/playback_bench: 100 ms to copy a
    // 3840x1920 RGBA frame, vs 1 ms for the stitch itself).
    bool makeReadbackBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                            VkDeviceMemory& mem, void** map)
    {
        const VkMemoryPropertyFlags coherent =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (makeBuffer(size, usage, coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, buf, mem, map))
            return true;
        if (buf) {
            vkDestroyBuffer(device, buf, nullptr);
            buf = VK_NULL_HANDLE;
        }
        return makeBuffer(size, usage, coherent, buf, mem, map);
    }

    bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mprops,
                    VkBuffer& buf, VkDeviceMemory& mem, void** map)
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &buf) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buf, &req);
        uint32_t type = 0;
        if (!findMemoryType(req.memoryTypeBits, mprops, type))
            return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS)
            return false;
        vkBindBufferMemory(device, buf, mem, 0);
        if (map)
            vkMapMemory(device, mem, 0, size, 0, map);
        return true;
    }

    void destroyResources()
    {
        for (int i = 0; i < 2; ++i) {
            if (fisheyeView[i]) vkDestroyImageView(device, fisheyeView[i], nullptr);
            if (fisheye[i]) vkDestroyImage(device, fisheye[i], nullptr);
            if (fisheyeMem[i]) vkFreeMemory(device, fisheyeMem[i], nullptr);
            fisheyeView[i] = VK_NULL_HANDLE;
            fisheye[i] = VK_NULL_HANDLE;
            fisheyeMem[i] = VK_NULL_HANDLE;
        }
        auto killBuf = [&](VkBuffer& b, VkDeviceMemory& m, void*& mp) {
            if (mp) { vkUnmapMemory(device, m); mp = nullptr; }
            if (b) vkDestroyBuffer(device, b, nullptr);
            if (m) vkFreeMemory(device, m, nullptr);
            b = VK_NULL_HANDLE;
            m = VK_NULL_HANDLE;
        };
        killBuf(staging, stagingMem, stagingMap);
        killBuf(ubo, uboMem, uboMap);
        killBuf(outBuf, outMem, outMap);
        killBuf(gainBuf, gainMem, gainMap);
        killBuf(seamBuf, seamMem, seamMap);
        killBuf(flatBuf, flatMem, flatMap);
        fw = fh = ew = eh = 0;
        flatBytes = 0;
    }

    void tsReset()
    {
        if (tsPool)
            vkCmdResetQueryPool(cmd, tsPool, 0, 3);
    }
    void tsWrite(VkPipelineStageFlagBits stage, uint32_t query)
    {
        if (tsPool)
            vkCmdWriteTimestamp(cmd, stage, tsPool, query);
    }
    // After the fence: fill timing.stitchGpuMs (and reframeGpuMs if `reframe`).
    void tsRead(bool reframe)
    {
        timing.stitchGpuMs = timing.reframeGpuMs = -1.0;
        if (!tsPool)
            return;
        uint64_t t[3] = {};
        const uint32_t n = reframe ? 3 : 2;
        if (vkGetQueryPoolResults(device, tsPool, 0, n, sizeof t, t, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        timing.stitchGpuMs = (t[1] - t[0]) * tsPeriodNs * 1e-6;
        if (reframe)
            timing.reframeGpuMs = (t[2] - t[1]) * tsPeriodNs * 1e-6;
    }

    // fill + measure pass + stitch pass, into the already-begun `cmd`.
    void recordStitchPasses(int eqW, int eqH)
    {
        vkCmdFillBuffer(cmd, gainBuf, 0, 12, 0);
        VkBufferMemoryBarrier zb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        zb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        zb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        zb.srcQueueFamilyIndex = zb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        zb.buffer = gainBuf;
        zb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &zb, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &descSet, 0,
                                nullptr);
        const uint32_t gx = (static_cast<uint32_t>(eqW) + kLocalSize - 1) / kLocalSize;
        const uint32_t gy = (static_cast<uint32_t>(eqH) + kLocalSize - 1) / kLocalSize;

        int32_t pass = 0;
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
        vkCmdDispatch(cmd, gx, gy, 1);

        VkBufferMemoryBarrier mb = zb;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &mb, 0, nullptr);

        pass = 1;
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
        vkCmdDispatch(cmd, gx, gy, 1);
    }

    // nv12 mode: a cached ycbcr-conversion view over one of the decoder's
    // pooled 2-plane images.
    VkImageView nv12ViewFor(VkImage img, VkFormat fmt)
    {
        auto it = nv12Views.find(img);
        if (it != nv12Views.end())
            return it->second;
        // The decoder's images carry STORAGE (and video) usage, which the
        // 2-plane format can't back in a view: sample-only views.
        VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkSamplerYcbcrConversionInfo yci{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
        yci.pNext = &usage;
        yci.conversion = ycbcr;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.pNext = &yci;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        nv12Views.emplace(img, view);
        return view;
    }

    // UBO + gain buffer + seam cost buffer, created once, and bound (0, 4
    // and 5) in both the stitch set and the view set.
    bool ensureUbo(std::string& err)
    {
        if (ubo)
            return true;
        const VkMemoryPropertyFlags hostVis =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!makeBuffer(sizeof(ShaderParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostVis, ubo,
                        uboMem, &uboMap) ||
            !makeBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        hostVis, gainBuf, gainMem, &gainMap) ||
            !makeReadbackBuffer(kSeamBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, seamBuf, seamMem,
                                &seamMap)) {
            err = "uniform / gain / seam buffer allocation failed";
            return false;
        }
        VkDescriptorBufferInfo uboInfo{ubo, 0, sizeof(ShaderParams)};
        VkDescriptorBufferInfo gainInfo{gainBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo seamInfo{seamBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[6]{};
        int n = 0;
        for (VkDescriptorSet set : {descSet, vwSet}) {
            if (!set)
                continue;
            w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[n].dstSet = set;
            w[n].dstBinding = 0;
            w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[n].pBufferInfo = &uboInfo;
            ++n;
            w[n] = w[n - 1];
            w[n].dstBinding = 4;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[n].pBufferInfo = &gainInfo;
            ++n;
            w[n] = w[n - 1];
            w[n].dstBinding = 5;
            w[n].pBufferInfo = &seamInfo;
            ++n;
        }
        vkUpdateDescriptorSets(device, n, w, 0, nullptr);
        return true;
    }

    // The equirect output SSBO (stitch set binding 3), plus ensureUbo(). Both
    // modes.
    bool ensureOutput(int new_, int neh, std::string& err)
    {
        if (!ensureUbo(err))
            return false;
        if (new_ == ew && neh == eh && outBuf)
            return true;
        idleBeforeFree();
        if (outMap) { vkUnmapMemory(device, outMem); outMap = nullptr; }
        if (outBuf) vkDestroyBuffer(device, outBuf, nullptr);
        if (outMem) vkFreeMemory(device, outMem, nullptr);
        outBuf = VK_NULL_HANDLE;
        outMem = VK_NULL_HANDLE;
        if (!makeReadbackBuffer(static_cast<VkDeviceSize>(new_) * neh * 4,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, outBuf, outMem, &outMap)) {
            err = "host-visible buffer allocation failed";
            return false;
        }
        VkDescriptorBufferInfo outInfo{outBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descSet;
        w.dstBinding = 3;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &outInfo;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        ew = new_;
        eh = neh;
        return true;
    }

    // One exportable image with its own dedicated memory, and the memory's FD.
    bool makeExportImage(VkFormat fmt, int w, int h, VkImage& img, VkDeviceMemory& mem,
                         uint64_t& size, int& fd, std::string& err)
    {
        VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.pNext = &emi;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ii, nullptr, &img) != VK_SUCCESS) {
            err = "vkCreateImage (exportable) failed";
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, img, &req);
        uint32_t type = 0;
        if (!findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
            err = "no device-local memory for an exportable image";
            return false;
        }
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = img;
        VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exp.pNext = &ded;
        exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &exp;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) {
            err = "vkAllocateMemory (exportable) failed";
            return false;
        }
        vkBindImageMemory(device, img, mem, 0);
        VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        gfi.memory = mem;
        gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (getMemoryFd(device, &gfi, &fd) != VK_SUCCESS) {
            err = "vkGetMemoryFdKHR failed";
            return false;
        }
        size = req.size;
        return true;
    }

    // A free ring slot for lensW x lensH lenses, marked busy -- reusing one,
    // allocating a new one, or waiting for one to come back.
    LensRingCore::Slot* acquireLensSlot(int lensW, int lensH, std::string& err)
    {
        if (!ring) {
            ring = std::make_shared<LensRingCore>();
            ring->device = device;
            ring->keepAlive = keepAlive;
        }
        std::unique_lock<std::mutex> lk(ring->mx);
        const auto deadline = std::chrono::steady_clock::now() + kLensSlotWait;
        for (;;) {
            for (auto& sl : ring->slots)
                if (!sl->busy && sl->pub.lensW == lensW && sl->pub.lensH == lensH) {
                    sl->busy = true;
                    return sl.get();
                }
            const size_t cap = std::chrono::steady_clock::now() < deadline ? kLensSlotsSoft
                                                                           : kLensSlotsHard;
            if (ring->slots.size() < cap)
                break;
            if (ring->slots.size() >= kLensSlotsHard &&
                std::chrono::steady_clock::now() >= deadline) {
                err = "lens ring exhausted (every slot held)";
                return nullptr;
            }
            ring->cv.wait_until(lk, deadline);
        }
        // Drop free slots of another size first (a source's lens size doesn't
        // change, but be tidy).
        for (auto it = ring->slots.begin(); it != ring->slots.end();) {
            if (!(*it)->busy && ((*it)->pub.lensW != lensW || (*it)->pub.lensH != lensH)) {
                ring->destroySlot(**it);
                it = ring->slots.erase(it);
            } else {
                ++it;
            }
        }
        auto sl = std::make_unique<LensRingCore::Slot>();
        sl->pub.lensW = lensW;
        sl->pub.lensH = lensH;
        sl->pub.id = LensRingCore::nextId.fetch_add(1);
        std::memcpy(sl->pub.deviceUuid, deviceUuid, sizeof deviceUuid);
        for (int i = 0; i < LensSlot::kImages; ++i) {
            const bool uv = (i & 1) != 0;
            if (!makeExportImage(uv ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM,
                                 uv ? lensW / 2 : lensW, uv ? lensH / 2 : lensH, sl->img[i],
                                 sl->mem[i], sl->pub.memSize[i], sl->pub.fd[i], err)) {
                ring->destroySlot(*sl);
                return nullptr;
            }
        }
        sl->busy = true;
        ring->slots.push_back(std::move(sl));
        return ring->slots.back().get();
    }

    bool ensureResources(int nfw, int nfh, int new_, int neh, std::string& err)
    {
        if (!ensureOutput(new_, neh, err))
            return false;
        if (nfw == fw && nfh == fh && fisheye[0])
            return true;
        idleBeforeFree();
        for (int i = 0; i < 2; ++i) {
            if (fisheyeView[i]) vkDestroyImageView(device, fisheyeView[i], nullptr);
            if (fisheye[i]) vkDestroyImage(device, fisheye[i], nullptr);
            if (fisheyeMem[i]) vkFreeMemory(device, fisheyeMem[i], nullptr);
            fisheyeView[i] = VK_NULL_HANDLE;
            fisheye[i] = VK_NULL_HANDLE;
            fisheyeMem[i] = VK_NULL_HANDLE;
        }
        if (stagingMap) { vkUnmapMemory(device, stagingMem); stagingMap = nullptr; }
        if (staging) vkDestroyBuffer(device, staging, nullptr);
        if (stagingMem) vkFreeMemory(device, stagingMem, nullptr);
        staging = VK_NULL_HANDLE;
        stagingMem = VK_NULL_HANDLE;

        const VkDeviceSize onePix = static_cast<VkDeviceSize>(nfw) * nfh;
        for (int i = 0; i < 2; ++i) {
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent = {static_cast<uint32_t>(nfw), static_cast<uint32_t>(nfh), 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device, &ii, nullptr, &fisheye[i]) != VK_SUCCESS) {
                err = "vkCreateImage failed";
                return false;
            }
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device, fisheye[i], &req);
            uint32_t type = 0;
            if (!findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
                err = "no device-local memory for fisheye image";
                return false;
            }
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            if (vkAllocateMemory(device, &ai, nullptr, &fisheyeMem[i]) != VK_SUCCESS) {
                err = "vkAllocateMemory (fisheye) failed";
                return false;
            }
            vkBindImageMemory(device, fisheye[i], fisheyeMem[i], 0);

            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = fisheye[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device, &vi, nullptr, &fisheyeView[i]) != VK_SUCCESS) {
                err = "vkCreateImageView failed";
                return false;
            }
        }

        const VkMemoryPropertyFlags hostVis =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!makeBuffer(onePix * 4 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVis, staging,
                        stagingMem, &stagingMap)) {
            err = "staging buffer allocation failed";
            return false;
        }

        // Point descriptor bindings 1 & 2 at the fixed upload images.
        VkDescriptorImageInfo img0{sampler, fisheyeView[0],
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{sampler, fisheyeView[1],
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = descSet;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        vkUpdateDescriptorSets(device, 2, w, 0, nullptr);

        fw = nfw;
        fh = nfh;
        return true;
    }
};

VulkanFisheyeStitcher::VulkanFisheyeStitcher() : d(new Impl) {}

VulkanFisheyeStitcher::~VulkanFisheyeStitcher()
{
    if (d && d->device) {
        // On an adopted device, idle only our queue -- FFmpeg may still be
        // using the device on another thread.
        if (d->adopted) {
            if (d->lockQueue)
                d->lockQueue();
            vkQueueWaitIdle(d->queue);
            if (d->unlockQueue)
                d->unlockQueue();
        } else
            vkDeviceWaitIdle(d->device);
        d->destroyResources();
        d->ring.reset(); // freed now, or by the last LensFrame still holding a slot
        for (auto& [img, view] : d->nv12Views)
            vkDestroyImageView(d->device, view, nullptr);
        d->nv12Views.clear();
        if (d->pipeline) vkDestroyPipeline(d->device, d->pipeline, nullptr);
        if (d->pipeLayout) vkDestroyPipelineLayout(d->device, d->pipeLayout, nullptr);
        if (d->descPool) vkDestroyDescriptorPool(d->device, d->descPool, nullptr);
        if (d->setLayout) vkDestroyDescriptorSetLayout(d->device, d->setLayout, nullptr);
        if (d->vwPipeline) vkDestroyPipeline(d->device, d->vwPipeline, nullptr);
        if (d->vwPipeLayout) vkDestroyPipelineLayout(d->device, d->vwPipeLayout, nullptr);
        if (d->rfPipeline) vkDestroyPipeline(d->device, d->rfPipeline, nullptr);
        if (d->rfPipeLayout) vkDestroyPipelineLayout(d->device, d->rfPipeLayout, nullptr);
        if (d->rfDescPool) vkDestroyDescriptorPool(d->device, d->rfDescPool, nullptr);
        if (d->rfSetLayout) vkDestroyDescriptorSetLayout(d->device, d->rfSetLayout, nullptr);
        if (d->sampler) vkDestroySampler(d->device, d->sampler, nullptr);
        if (d->ycbcr) vkDestroySamplerYcbcrConversion(d->device, d->ycbcr, nullptr);
        if (d->tsPool) vkDestroyQueryPool(d->device, d->tsPool, nullptr);
        if (d->fence) vkDestroyFence(d->device, d->fence, nullptr);
        if (d->cmdPool) vkDestroyCommandPool(d->device, d->cmdPool, nullptr);
        if (!d->adopted)
            vkDestroyDevice(d->device, nullptr);
    }
    if (d && d->instance && !d->adopted)
        vkDestroyInstance(d->instance, nullptr);
    delete d;
}

bool VulkanFisheyeStitcher::init(std::string& err)
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "orbis-reframe-stitch";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char* valLayer = "VK_LAYER_KHRONOS_validation";
    if (std::getenv("INSV_VK_VALIDATE")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &valLayer;
    }
    if (vkCreateInstance(&ici, nullptr, &d->instance) != VK_SUCCESS) {
        err = "vkCreateInstance failed";
        return false;
    }

    uint32_t nphys = 0;
    vkEnumeratePhysicalDevices(d->instance, &nphys, nullptr);
    if (nphys == 0) {
        err = "no Vulkan physical devices";
        return false;
    }
    std::vector<VkPhysicalDevice> devs(nphys);
    vkEnumeratePhysicalDevices(d->instance, &nphys, devs.data());
    d->phys = devs[0];
    for (VkPhysicalDevice pd : devs) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            d->phys = pd;
            break;
        }
    }
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memProps);

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nqf, qfs.data());
    bool found = false;
    for (uint32_t i = 0; i < nqf; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            d->queueFamily = i;
            found = true;
            break;
        }
    }
    if (!found) {
        err = "no compute queue family";
        return false;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d->queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(d->phys, &dci, nullptr, &d->device) != VK_SUCCESS) {
        err = "vkCreateDevice failed";
        return false;
    }
    vkGetDeviceQueue(d->device, d->queueFamily, 0, &d->queue);

    return bringUp(false, err);
}

bool VulkanFisheyeStitcher::initAdopt(VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                                      uint32_t computeQueueFamily, std::string& err)
{
    d->adopted = true;
    d->instance = inst;
    d->phys = phys;
    d->device = dev;
    d->queueFamily = computeQueueFamily;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memProps);
    vkGetDeviceQueue(d->device, d->queueFamily, 0, &d->queue);
    // Present when FFmpeg enabled VK_KHR_external_memory_fd (it does, for its
    // own interop); null => no lens export, playback stays on the equirect.
    d->getMemoryFd =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR"));
    {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(phys, &p2);
        std::memcpy(d->deviceUuid, idp.deviceUUID, sizeof d->deviceUuid);
    }
    return bringUp(true, err);
}

bool VulkanFisheyeStitcher::bringUp(bool nv12Mode, std::string& err)
{
    d->nv12 = nv12Mode;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d->queueFamily;
    if (vkCreateCommandPool(d->device, &pci, nullptr, &d->cmdPool) != VK_SUCCESS) {
        err = "vkCreateCommandPool failed";
        return false;
    }

    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = d->cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(d->device, &cbi, &d->cmd);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(d->device, &fci, nullptr, &d->fence);

    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nq, qf.data());
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d->phys, &props);
        if (d->queueFamily < nq && qf[d->queueFamily].timestampValidBits > 0 &&
            props.limits.timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = 3;
            if (vkCreateQueryPool(d->device, &qpi, nullptr, &d->tsPool) == VK_SUCCESS)
                d->tsPeriodNs = props.limits.timestampPeriod;
        }
    }

    VkSamplerYcbcrConversionInfo yci{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    if (nv12Mode) {
        VkSamplerYcbcrConversionCreateInfo ycci{
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
        ycci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; // NV12
        ycci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        ycci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL; // camera HEVC is full range
        ycci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        ycci.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ycci.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ycci.chromaFilter = VK_FILTER_LINEAR;
        if (vkCreateSamplerYcbcrConversion(d->device, &ycci, nullptr, &d->ycbcr) != VK_SUCCESS) {
            err = "vkCreateSamplerYcbcrConversion failed (device lacks samplerYcbcrConversion)";
            return false;
        }
        yci.conversion = d->ycbcr;
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    if (nv12Mode)
        si.pNext = &yci;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(d->device, &si, nullptr, &d->sampler) != VK_SUCCESS) {
        err = "vkCreateSampler failed";
        return false;
    }

    // Bindings 1 & 2 use an immutable sampler in nv12 mode (ycbcr conversion
    // must be baked into the layout).
    const VkSampler immutable[2] = {d->sampler, d->sampler};
    VkDescriptorSetLayoutBinding b[6]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
            nv12Mode ? &immutable[0] : nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
            nv12Mode ? &immutable[1] : nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 6;
    sli.pBindings = b;
    vkCreateDescriptorSetLayout(d->device, &sli, nullptr, &d->setLayout);

    // Two sets of this layout: the stitch's, and the fused view pass's.
    VkDescriptorPoolSize ps[3]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                               {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = ps;
    vkCreateDescriptorPool(d->device, &dpi, nullptr, &d->descPool);

    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = d->descPool;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &d->setLayout;
    vkAllocateDescriptorSets(d->device, &dsi, &d->descSet);

    const shaders::Spirv spv =
        nv12Mode ? shaders::fisheyeStitchNv12Comp() : shaders::fisheyeStitchComp();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.bytes;
    smi.pCode = spv.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &smi, nullptr, &module) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed";
        return false;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4}; // int pass
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d->setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(d->device, &pli, nullptr, &d->pipeLayout);

    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = d->pipeLayout;
    const VkResult pr =
        vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d->pipeline);
    vkDestroyShaderModule(d->device, module, nullptr);
    if (pr != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed";
        return false;
    }

    // The export reproject pass is optional -- a build without the SPV, or a
    // pipeline-creation failure, just leaves reframeReady() false and callers
    // fall back to the CPU reproject.
    std::string rferr;
    if (!buildReframePipeline(rferr))
        std::fprintf(stderr, "[VulkanFisheyeStitcher] reframe reproject pass unavailable (%s); "
                             "export will reproject on the CPU\n",
                     rferr.c_str());
    // Likewise the fused fisheye -> view pass (NV12 inputs only).
    if (nv12Mode) {
        std::string vwerr;
        if (!buildViewPipeline(vwerr))
            std::fprintf(stderr, "[VulkanFisheyeStitcher] fused view pass unavailable (%s)\n",
                         vwerr.c_str());
    }

    m_ready = true;
    return true;
}

bool VulkanFisheyeStitcher::buildReframePipeline(std::string& err)
{

    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 2;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->device, &sli, nullptr, &d->rfSetLayout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout (reframe) failed";
        return false;
    }

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d->device, &dpi, nullptr, &d->rfDescPool) != VK_SUCCESS) {
        err = "vkCreateDescriptorPool (reframe) failed";
        return false;
    }
    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = d->rfDescPool;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &d->rfSetLayout;
    if (vkAllocateDescriptorSets(d->device, &dsi, &d->rfSet) != VK_SUCCESS) {
        err = "vkAllocateDescriptorSets (reframe) failed";
        return false;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReframePush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d->rfSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d->device, &pli, nullptr, &d->rfPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout (reframe) failed";
        return false;
    }

    const shaders::Spirv spv = shaders::reframeReprojectComp();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.bytes;
    smi.pCode = spv.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &smi, nullptr, &module) != VK_SUCCESS) {
        err = "vkCreateShaderModule (reframe) failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = d->rfPipeLayout;
    const VkResult pr =
        vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d->rfPipeline);
    vkDestroyShaderModule(d->device, module, nullptr);
    if (pr != VK_SUCCESS) {
        err = "vkCreateComputePipelines (reframe) failed";
        return false;
    }

    m_reframeReady = true;
    return true;
}

bool VulkanFisheyeStitcher::buildViewPipeline(std::string& err)
{
    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = d->descPool;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &d->setLayout;
    if (vkAllocateDescriptorSets(d->device, &dsi, &d->vwSet) != VK_SUCCESS) {
        err = "vkAllocateDescriptorSets (view) failed";
        return false;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ViewPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d->setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d->device, &pli, nullptr, &d->vwPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout (view) failed";
        return false;
    }

    const shaders::Spirv spv = shaders::fisheyeViewNv12Comp();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.bytes;
    smi.pCode = spv.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &smi, nullptr, &module) != VK_SUCCESS) {
        err = "vkCreateShaderModule (view) failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = d->vwPipeLayout;
    const VkResult pr =
        vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d->vwPipeline);
    vkDestroyShaderModule(d->device, module, nullptr);
    if (pr != VK_SUCCESS) {
        err = "vkCreateComputePipelines (view) failed";
        return false;
    }
    m_viewReady = true;
    return true;
}

bool VulkanFisheyeStitcher::stitch(const uint8_t* rgb0, const uint8_t* rgb1,
                                   const insv::StitchGpuParams& params, int eqW, int eqH,
                                   std::vector<uint8_t>& outRgba, std::string& err)
{
    if (!m_ready) {
        err = "stitcher not initialised";
        return false;
    }
    const int fwv = params.fisheyeW;
    const int fhv = params.fisheyeH;
    if (fwv <= 0 || fhv <= 0 || eqW <= 0 || eqH <= 0) {
        err = "bad dimensions";
        return false;
    }
    if (!d->ensureResources(fwv, fhv, eqW, eqH, err))
        return false;

    const size_t oneRgba = static_cast<size_t>(fwv) * fhv * 4;
    expandRgbToRgba(rgb0, static_cast<uint8_t*>(d->stagingMap), fwv, fhv);
    expandRgbToRgba(rgb1, static_cast<uint8_t*>(d->stagingMap) + oneRgba, fwv, fhv);

    ShaderParams sp{};
    packParams(params, eqW, eqH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->tsReset();

    VkImageMemoryBarrier toDst[2]{};
    for (int i = 0; i < 2; ++i) {
        toDst[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst[i].image = d->fisheye[i];
        toDst[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toDst[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 2, toDst);

    for (int i = 0; i < 2; ++i) {
        VkBufferImageCopy region{};
        region.bufferOffset = i == 0 ? 0 : oneRgba;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(fwv), static_cast<uint32_t>(fhv), 1};
        vkCmdCopyBufferToImage(d->cmd, d->staging, d->fisheye[i],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = toDst[i];
        toRead[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    d->recordStitchPasses(eqW, eqH);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->outBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &toHost, 0, nullptr);

    vkEndCommandBuffer(d->cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }
    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(d->device, 1, &d->fence);

    const size_t outBytes = static_cast<size_t>(eqW) * eqH * 4;
    outRgba.resize(outBytes);
    std::memcpy(outRgba.data(), d->outMap, outBytes);
    return true;
}

bool VulkanFisheyeStitcher::stitchVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                     const insv::StitchGpuParams& params, int eqW, int eqH,
                                     std::vector<uint8_t>& outRgba, VulkanLensOutcome* r0,
                                     VulkanLensOutcome* r1, std::string& err)
{
    if (!m_ready || !d->nv12) {
        err = "stitchVk requires initAdopt()";
        return false;
    }
    if (eqW <= 0 || eqH <= 0) {
        err = "bad equirect dimensions";
        return false;
    }
    if (!d->ensureOutput(eqW, eqH, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    ShaderParams sp{};
    packParams(params, eqW, eqH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = d->descSet;
    w[0].dstBinding = 1;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &img0;
    w[1] = w[0];
    w[1].dstBinding = 2;
    w[1].pImageInfo = &img1;
    vkUpdateDescriptorSets(d->device, 2, w, 0, nullptr);

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->tsReset();

    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead[i].oldLayout = in[i]->layout;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = 0; // covered by the semaphore wait
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = in[i]->image;
        toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    d->recordStitchPasses(eqW, eqH);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->outBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &toHost, 0, nullptr);

    vkEndCommandBuffer(d->cmd);

    // Wait on both decode semaphores at their current value; signal each +1.
    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    const auto tSubmit = std::chrono::steady_clock::now();
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }

    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);
    const auto tDone = std::chrono::steady_clock::now();
    d->tsRead(false);

    const size_t outBytes = static_cast<size_t>(eqW) * eqH * 4;
    outRgba.resize(outBytes);
    std::memcpy(outRgba.data(), d->outMap, outBytes);
    d->timing.fenceWaitMs = msBetween(tSubmit, tDone);
    d->timing.copyMs = msBetween(tDone, std::chrono::steady_clock::now());
    return true;
}

bool VulkanFisheyeStitcher::stitchAndReframeVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                               const insv::StitchGpuParams& params, int eqW, int eqH,
                                               int outW, int outH, const float pose[4],
                                               const float stab[9], const float colorGrade[4],
                                               std::vector<uint8_t>& outNv12, VulkanLensOutcome* r0,
                                               VulkanLensOutcome* r1, std::string& err)
{
    if (!m_ready || !d->nv12) {
        err = "stitchAndReframeVk requires initAdopt()";
        return false;
    }
    if (!m_reframeReady) {
        err = "reframe reproject pipeline not built";
        return false;
    }
    if (eqW <= 0 || eqH <= 0 || outW <= 0 || outH <= 0 || (outW & 3) != 0 || (outH & 1) != 0) {
        err = "bad dimensions (outW must be a multiple of 4, outH a multiple of 2)";
        return false;
    }
    if (!d->ensureOutput(eqW, eqH, err) ||
        !d->ensureFlat(static_cast<VkDeviceSize>(outW) * outH * 3 / 2, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    ShaderParams sp{};
    packParams(params, eqW, eqH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    // Stitch set: point the lens bindings at this pair.
    {
        VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = d->descSet;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        vkUpdateDescriptorSets(d->device, 2, w, 0, nullptr);
    }
    // Reproject set: equirect OutBuf in, NV12 flatBuf out.
    {
        VkDescriptorBufferInfo eqInfo{d->outBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo flatInfo{d->flatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = d->rfSet;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &eqInfo;
        w[1] = w[0];
        w[1].dstBinding = 1;
        w[1].pBufferInfo = &flatInfo;
        vkUpdateDescriptorSets(d->device, 2, w, 0, nullptr);
    }

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->tsReset();

    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead[i].oldLayout = in[i]->layout;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = 0; // covered by the semaphore wait
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = in[i]->image;
        toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    d->recordStitchPasses(eqW, eqH);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    // Equirect just written by the stitch -> readable by the reproject.
    VkBufferMemoryBarrier eqRW{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    eqRW.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    eqRW.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    eqRW.srcQueueFamilyIndex = eqRW.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    eqRW.buffer = d->outBuf;
    eqRW.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &eqRW, 0, nullptr);

    ReframePush rp{};
    rp.eqW = eqW;
    rp.eqH = eqH;
    rp.outW = outW;
    rp.outH = outH;
    rp.yaw = pose ? pose[0] : 0.0f;
    rp.pitch = pose ? pose[1] : 0.0f;
    rp.roll = pose ? pose[2] : 0.0f;
    rp.fovY = pose ? pose[3] : 1.309f;
    rp.aspect = static_cast<float>(outW) / static_cast<float>(outH);
    // Row-major stab -> col-major padded vec3 columns; nullptr => identity.
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            rp.stab[col * 4 + row] = stab ? stab[row * 3 + col] : (row == col ? 1.0f : 0.0f);
    for (int i = 0; i < 4; ++i)
        rp.colorGrade[i] = colorGrade ? colorGrade[i] : 0.0f;

    vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->rfPipeline);
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->rfPipeLayout, 0, 1, &d->rfSet,
                            0, nullptr);
    vkCmdPushConstants(d->cmd, d->rfPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
    const uint32_t gx = (static_cast<uint32_t>(outW / 4) + 15) / 16;
    const uint32_t gy = (static_cast<uint32_t>(outH) + 3) / 4;
    vkCmdDispatch(d->cmd, gx, gy, 1);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->flatBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &toHost, 0, nullptr);

    vkEndCommandBuffer(d->cmd);

    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    const auto tSubmit = std::chrono::steady_clock::now();
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }

    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);
    const auto tDone = std::chrono::steady_clock::now();
    d->tsRead(true);

    const size_t nbytes = static_cast<size_t>(outW) * outH * 3 / 2;
    outNv12.resize(nbytes);
    std::memcpy(outNv12.data(), d->flatMap, nbytes);
    d->timing.fenceWaitMs = msBetween(tSubmit, tDone);
    d->timing.copyMs = msBetween(tDone, std::chrono::steady_clock::now());
    return true;
}

bool VulkanFisheyeStitcher::renderViewVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                         const insv::StitchGpuParams& params, int measureW,
                                         int measureH, int outW, int outH, const float pose[4],
                                         const float stab[9], const float colorGrade[4],
                                         ViewFormat fmt, std::vector<uint8_t>& out,
                                         VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                         std::string& err)
{
    ViewRect v;
    if (pose)
        std::copy(pose, pose + 4, v.pose);
    v.w = outW;
    v.h = outH;
    return renderViewsImpl(f0, f1, params, measureW, measureH, 0, 0, {v}, stab, colorGrade, fmt,
                           nullptr, out, r0, r1, err);
}

bool VulkanFisheyeStitcher::renderViewsVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                          const insv::StitchGpuParams& params, int measureW,
                                          int measureH, int canvasW, int canvasH,
                                          const std::vector<ViewRect>& views, const float stab[9],
                                          uint32_t clearRgba, std::vector<uint8_t>& out,
                                          VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                          std::string& err)
{
    if (canvasW <= 0 || canvasH <= 0 || (canvasW & 3) != 0 || views.empty()) {
        err = "bad canvas (width a multiple of 4, at least one view)";
        return false;
    }
    for (const ViewRect& v : views)
        if (v.x < 0 || v.y < 0 || (v.x & 3) != 0 || v.x + v.w > canvasW || v.y + v.h > canvasH) {
            err = "view outside the canvas (or x not a multiple of 4)";
            return false;
        }
    return renderViewsImpl(f0, f1, params, measureW, measureH, canvasW, canvasH, views, stab,
                           nullptr, ViewFormat::Rgba, &clearRgba, out, r0, r1, err);
}

bool VulkanFisheyeStitcher::renderViewsImpl(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                            const insv::StitchGpuParams& params, int measureW,
                                            int measureH, int canvasW, int canvasH,
                                            const std::vector<ViewRect>& views,
                                            const float stab[9], const float colorGrade[4],
                                            ViewFormat fmt, const uint32_t* clear,
                                            std::vector<uint8_t>& out, VulkanLensOutcome* r0,
                                            VulkanLensOutcome* r1, std::string& err)
{
    if (!m_ready || !d->nv12 || !m_viewReady) {
        err = "renderViewVk requires initAdopt() and the fused view pipeline";
        return false;
    }
    // A single view is its own canvas.
    const bool single = canvasW == 0;
    const int outW = single ? views[0].w : canvasW;
    const int outH = single ? views[0].h : canvasH;
    for (const ViewRect& v : views)
        if (v.w <= 0 || v.h <= 0 || (v.w & 3) != 0 || (v.h & 1) != 0) {
            err = "bad dimensions (outW must be a multiple of 4, outH a multiple of 2)";
            return false;
        }
    if (measureW <= 0 || measureH <= 0 || outW <= 0 || outH <= 0) {
        err = "bad dimensions (outW must be a multiple of 4, outH a multiple of 2)";
        return false;
    }
    const VkDeviceSize outBytes = fmt == ViewFormat::Rgba
                                      ? static_cast<VkDeviceSize>(outW) * outH * 4
                                      : static_cast<VkDeviceSize>(outW) * outH * 3 / 2;
    if (!d->ensureUbo(err) || !d->ensureFlat(outBytes, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    // dims.zw = the measure grid: the equirect the stitch would have made, so
    // the photometric gains come out the same as on the equirect path.
    ShaderParams sp{};
    packParams(params, measureW, measureH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    {
        VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo flatInfo{d->flatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = d->vwSet;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[2].dstSet = d->vwSet;
        w[2].dstBinding = 3;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &flatInfo;
        vkUpdateDescriptorSets(d->device, 3, w, 0, nullptr);
    }

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->tsReset();

    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead[i].oldLayout = in[i]->layout;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = 0; // covered by the semaphore wait
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = in[i]->image;
        toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);

    vkCmdFillBuffer(d->cmd, d->gainBuf, 0, 12, 0);
    if (clear) // the canvas background, where no view goes
        vkCmdFillBuffer(d->cmd, d->flatBuf, 0, outBytes, *clear);
    VkBufferMemoryBarrier zb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    zb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    zb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    zb.srcQueueFamilyIndex = zb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    zb.buffer = d->gainBuf;
    zb.size = VK_WHOLE_SIZE;
    VkBufferMemoryBarrier fb = zb;
    fb.buffer = d->flatBuf;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, clear ? 2 : 1,
                         clear ? std::array<VkBufferMemoryBarrier, 2>{zb, fb}.data() : &zb, 0,
                         nullptr);

    vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeline);
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeLayout, 0, 1,
                            &d->vwSet, 0, nullptr);

    ViewPush vp{};
    vp.outW = outW;
    vp.outH = outH;
    vp.fmt = fmt == ViewFormat::Rgba ? 1 : 0;
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            vp.stab[col * 4 + row] = stab ? stab[row * 3 + col] : (row == col ? 1.0f : 0.0f);
    for (int i = 0; i < 4; ++i)
        vp.colorGrade[i] = colorGrade ? colorGrade[i] : 0.0f;

    // Pass 0: one invocation per measure-grid point (every 8th texel).
    vp.pass = 0;
    vkCmdPushConstants(d->cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
    const uint32_t mgx = (static_cast<uint32_t>(measureW) + 7) / 8;
    const uint32_t mgy = (static_cast<uint32_t>(measureH) + 7) / 8;
    vkCmdDispatch(d->cmd, (mgx + 15) / 16, (mgy + 3) / 4, 1);

    VkBufferMemoryBarrier mb = zb;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &mb, 0, nullptr);

    // Pass 1: each view, 4 pixels per invocation.
    vp.pass = 1;
    for (const ViewRect& v : views) {
        vp.outW = v.w;
        vp.outH = v.h;
        vp.yaw = v.pose[0];
        vp.pitch = v.pose[1];
        vp.roll = v.pose[2];
        vp.fovY = v.pose[3];
        vp.aspect = static_cast<float>(v.w) / static_cast<float>(v.h);
        vp.ox = single ? 0 : v.x;
        vp.oy = single ? 0 : v.y;
        vp.stride = single ? 0 : canvasW;
        vkCmdPushConstants(d->cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
        vkCmdDispatch(d->cmd, (static_cast<uint32_t>(v.w / 4) + 15) / 16,
                      (static_cast<uint32_t>(v.h) + 3) / 4, 1);
    }
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->flatBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &toHost, 0, nullptr);

    vkEndCommandBuffer(d->cmd);

    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    const auto tSubmit = std::chrono::steady_clock::now();
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }

    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);
    const auto tDone = std::chrono::steady_clock::now();
    d->tsRead(false);

    out.resize(static_cast<size_t>(outBytes));
    std::memcpy(out.data(), d->flatMap, static_cast<size_t>(outBytes));
    d->timing.fenceWaitMs = msBetween(tSubmit, tDone);
    d->timing.copyMs = msBetween(tDone, std::chrono::steady_clock::now());
    return true;
}

bool VulkanFisheyeStitcher::measureSeamVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                          const insv::StitchGpuParams& params,
                                          insv::SeamCosts& out, VulkanLensOutcome* r0,
                                          VulkanLensOutcome* r1, std::string& err)
{
    if (!m_ready || !d->nv12) {
        err = "measureSeamVk requires initAdopt()";
        return false;
    }
    // Pass 2 lives in both the stitch and the fused view shader. Prefer the
    // view pipeline: its binding 3 only needs a token buffer, where the
    // stitch's wants the whole equirect.
    const bool viaView = m_viewReady;
    if (!d->ensureUbo(err) ||
        !(viaView ? d->ensureFlat(16, err) : d->ensureOutput(std::max(d->ew, 16), std::max(d->eh, 8), err)))
        return false;
    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }
    ShaderParams sp{};
    packParams(params, 16, 8, sp); // no equirect involved; dims.zw unused by pass 2
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    const VkDescriptorSet set = viaView ? d->vwSet : d->descSet;
    {
        VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo flatInfo{d->flatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = set;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[2].dstSet = set;
        w[2].dstBinding = 3;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &flatInfo;
        vkUpdateDescriptorSets(d->device, viaView ? 3 : 2, w, 0, nullptr);
    }

    VkCommandBuffer cmd = d->cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead[i].oldLayout = in[i]->layout;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = 0; // covered by the semaphore wait
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = in[i]->image;
        toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, toRead);

    if (viaView) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeLayout, 0, 1, &set, 0,
                                nullptr);
        ViewPush vp{};
        vp.pass = 2;
        vkCmdPushConstants(cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeLayout, 0, 1, &set, 0,
                                nullptr);
        const int32_t pass = 2;
        vkCmdPushConstants(cmd, d->pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
    }
    // One 64-invocation workgroup per (bin, candidate).
    vkCmdDispatch(cmd, insv::kSeamDepthBins, insv::kSeamDepthCands, 1);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->seamBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHost, 0, nullptr);
    vkEndCommandBuffer(cmd);

    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }
    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);

    const float* c = static_cast<const float*>(d->seamMap);
    for (int b = 0; b < insv::kSeamDepthBins; ++b)
        for (int k = 0; k < insv::kSeamDepthCands; ++k) {
            const float* v = c + 2 * (b * insv::kSeamDepthCands + k);
            out.ncc[b][k] = v[0];
            out.texture[b][k] = v[1];
        }
    return true;
}

bool VulkanFisheyeStitcher::lensExportReady() const
{
    return m_ready && d->nv12 && m_viewReady && d->getMemoryFd;
}

void VulkanFisheyeStitcher::setQueueLock(std::function<void()> lock, std::function<void()> unlock)
{
    d->lockQueue = std::move(lock);
    d->unlockQueue = std::move(unlock);
}

void VulkanFisheyeStitcher::setDeviceKeepAlive(std::shared_ptr<void> keepAlive)
{
    d->keepAlive = std::move(keepAlive);
    if (d->ring)
        d->ring->keepAlive = d->keepAlive;
}

void VulkanFisheyeStitcher::trimLensRing()
{
    if (!d->ring)
        return;
    std::lock_guard<std::mutex> lk(d->ring->mx);
    auto& slots = d->ring->slots;
    for (auto it = slots.begin(); it != slots.end();) {
        if (!(*it)->busy) {
            d->ring->destroySlot(**it);
            it = slots.erase(it);
        } else {
            ++it;
        }
    }
}

bool VulkanFisheyeStitcher::fillLensFrameVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                            const insv::StitchGpuParams& params, int measureW,
                                            int measureH, LensFrame& out, VulkanLensOutcome* r0,
                                            VulkanLensOutcome* r1, std::string& err)
{
    if (!lensExportReady()) {
        err = "fillLensFrameVk requires initAdopt(), the view pipeline and external memory";
        return false;
    }
    if (!(f0.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !(f1.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        err = "decoder images can't be copied from (no TRANSFER_SRC usage)";
        return false;
    }
    const int lw = f0.width, lh = f0.height;
    if (lw <= 0 || lh <= 0 || f1.width != lw || f1.height != lh || (lw & 1) || (lh & 1) ||
        measureW <= 0 || measureH <= 0) {
        err = "bad lens / measure dimensions";
        return false;
    }
    // Binding 3 of the view set must point somewhere even though pass 0
    // doesn't write it.
    if (!d->ensureUbo(err) || !d->ensureFlat(16, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    LensRingCore::Slot* slot = d->acquireLensSlot(lw, lh, err);
    if (!slot)
        return false;
    // Hand the slot back if anything below fails.
    struct Release {
        LensRingCore* ring;
        LensRingCore::Slot* slot;
        bool armed = true;
        ~Release()
        {
            if (!armed)
                return;
            std::lock_guard<std::mutex> lk(ring->mx);
            slot->busy = false;
            ring->cv.notify_all();
        }
    } release{d->ring.get(), slot};

    ShaderParams sp{};
    packParams(params, measureW, measureH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));
    {
        VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo flatInfo{d->flatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = d->vwSet;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[2].dstSet = d->vwSet;
        w[2].dstBinding = 3;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &flatInfo;
        vkUpdateDescriptorSets(d->device, 3, w, 0, nullptr);
    }

    VkCommandBuffer cmd = d->cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    d->tsReset();

    auto imgBarrier = [](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                         VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return b;
    };

    // Decoded pair -> sampled, for the measure pass.
    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2];
    for (int i = 0; i < 2; ++i)
        toRead[i] = imgBarrier(in[i]->image, in[i]->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               0, VK_ACCESS_SHADER_READ_BIT); // src covered by the semaphore wait
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);

    // Photometric measure: the view pipeline's pass 0 over the equirect grid.
    vkCmdFillBuffer(cmd, d->gainBuf, 0, 12, 0);
    VkBufferMemoryBarrier zb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    zb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    zb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    zb.srcQueueFamilyIndex = zb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    zb.buffer = d->gainBuf;
    zb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &zb, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeLayout, 0, 1, &d->vwSet,
                            0, nullptr);
    ViewPush vp{};
    vp.pass = 0;
    vkCmdPushConstants(cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
    const uint32_t mgx = (static_cast<uint32_t>(measureW) + 7) / 8;
    const uint32_t mgy = (static_cast<uint32_t>(measureH) + 7) / 8;
    vkCmdDispatch(cmd, (mgx + 15) / 16, (mgy + 3) / 4, 1);
    VkBufferMemoryBarrier toHostGain = zb;
    toHostGain.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHostGain.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHostGain, 0, nullptr);

    // Decoded pair -> transfer source; the slot's images -> transfer target
    // (old contents discarded, so no ownership acquire is needed).
    VkImageMemoryBarrier pre[2 + LensSlot::kImages];
    for (int i = 0; i < 2; ++i)
        pre[i] = imgBarrier(in[i]->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT);
    for (int i = 0; i < LensSlot::kImages; ++i)
        pre[2 + i] = imgBarrier(slot->img[i], VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         2 + LensSlot::kImages, pre);

    for (int lens = 0; lens < 2; ++lens) {
        for (int plane = 0; plane < 2; ++plane) {
            VkImageCopy c{};
            c.srcSubresource = {plane == 0 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT,
                                0, 0, 1};
            c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {static_cast<uint32_t>(plane == 0 ? lw : lw / 2),
                        static_cast<uint32_t>(plane == 0 ? lh : lh / 2), 1};
            vkCmdCopyImage(cmd, in[lens]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           slot->img[lens * 2 + plane], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    }

    // Release the slot's images to the importing device.
    VkImageMemoryBarrier rel[LensSlot::kImages];
    for (int i = 0; i < LensSlot::kImages; ++i) {
        rel[i] = imgBarrier(slot->img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        rel[i].srcQueueFamilyIndex = d->queueFamily;
        rel[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, LensSlot::kImages, rel);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
    vkEndCommandBuffer(cmd);

    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const auto tSubmit = std::chrono::steady_clock::now();
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }
    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }
    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);
    const auto tDone = std::chrono::steady_clock::now();
    d->tsRead(false);

    // The shader's photoGains(), on the CPU.
    const uint32_t* g = static_cast<const uint32_t*>(d->gainMap);
    constexpr float kGainFp = 4.0f;
    float g0 = 1.0f, g1 = 1.0f;
    if (params.photometric && g[2] > 64u) {
        const float m0 = float(g[0]) / (kGainFp * float(g[2]));
        const float m1 = float(g[1]) / (kGainFp * float(g[2]));
        if (m0 > 1.0f && m1 > 1.0f) {
            const float r = std::sqrt(m1 / m0);
            g0 = std::clamp(r, 0.8f, 1.25f);
            g1 = std::clamp(1.0f / r, 0.8f, 1.25f);
        }
    }

    out.fill = ++slot->fill;
    std::memcpy(out.params.data(), &sp, sizeof(sp));
    out.gain[0] = g0;
    out.gain[1] = g1;
    // The handle: hands the slot back to the ring when its last copy goes.
    std::shared_ptr<LensRingCore> ring = d->ring;
    out.slot = std::shared_ptr<const LensSlot>(&slot->pub, [ring, slot](const LensSlot*) {
        std::lock_guard<std::mutex> lk(ring->mx);
        slot->busy = false;
        ring->cv.notify_all();
    });
    release.armed = false;

    d->timing.fenceWaitMs = msBetween(tSubmit, tDone);
    d->timing.copyMs = 0.0;
    return true;
}

const StitchTiming& VulkanFisheyeStitcher::lastTiming() const
{
    return d->timing;
}

} // namespace render
