#pragma once

// VulkanFisheyeStitcher's insides, shared by its two source files: the
// per-frame passes (VulkanFisheyeStitcher.cpp) and the device, pipelines and
// resources they run on (VulkanFisheyeStitcherSetup.cpp). Private: only
// those two include it.

#include "VulkanFisheyeStitcher.h"
#include "VulkanLut.h"

#include <SeamColourBlend.h> // insv::SeamVeilFade, the new stitcher's fade
#include <SeamCut.h>         // insv::SeamCutStats, the cut pass's rows / patch
#include <SeamDepth.h>       // insv::SeamCosts, the cost pass's candidates / patch
#include <SeamFade.h>        // insv::SeamFadeRows, the fade passes' patches
#include <SeamFlow.h>        // insv::SeamFlowField, the seam flow texture

#include <vulkan/vulkan.h>

#include <unistd.h> // close() -- exported memory FDs

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace render {

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
    float seamC[4]; // cost pass: q min, q max (1/m), patch elevation half-range (rad); cut pass: row max (rad)
    float seamQ[insv::kSeamDepthBins]; // vec4[kSeamDepthBins / 4]: per-bin inverse distance
    float seamCF[insv::kSeamDepthBins][2]; // vec4[kSeamDepthBins / 2]: per-bin fade centre, half-width
    float lensCol[4];  // lens colour: log(lens 0 / lens 1) RGB (with the frame's residual), -
    float lensColS[4]; // its slope per radian of elevation, RGB, -
    float lensLocal[insv::kSeamDepthBins][4]; // per-bin local correction RGB, -
    float lensVeil[insv::kSeamDepthBins][4];  // per-bin veiling light RGB, -
    float seamCol[insv::kSeamDepthBins][4];   // per-bin seam colour blend RGB, - (active: edge.z)
    float seamFlow[4];    // seam flow: active, strip half-height (rad), reach (rad), -
    int32_t seamFlowN[4]; // its grid: columns, rows, -, -
    float blur0[4]; // privacy blur: count, -, -, -
    float blurQ[insv::kMaxBlurQuads * 6][4]; // per quad, see fisheye_stitch_common.glsl
    float ycc[3][4]; // lens Y'CbCr -> R'G'B' rows (insv::YcbcrToRgbRows), lens frames only
};
static_assert(sizeof(ShaderParams) == 14624, "ShaderParams must match the std140 UBO");
static_assert(sizeof(ShaderParams) == LensFrame::kParamsBytes, "LensFrame::params carries ShaderParams");
static_assert(LensSlot::kBlurCellsW == insv::kBlurCellsPerQuad && LensSlot::kBlurCellsH == insv::kMaxBlurQuads,
              "the blur cell table: a row of kBlurCellsPerQuad per quad");

// Push constants for reframe_reproject.comp. 4 ints + 8 words, then the
// stabilization matrix as three vec4 columns (a mat3's std430 layout); the
// first column's w is Natural (view::Lens::panini) -- the block is exactly
// Vulkan's guaranteed 128 bytes, so it rides in the padding.
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
    int32_t origin; // the view's top-left in a canvas of views: x | y << 16 (x % 4 == 0,
                    // y % 2 == 0) ...
    int32_t canvas; // ... and the canvas: width | height << 16; 0 = the view alone
    float distortion; // view::Pose::distortion
    float stab[12];      // 3 columns, each a vec3 padded to 16 bytes (col-major);
                         // stab[3] = Natural
    float colorGrade[4]; // exposureEv, contrast%, saturation%, temperature%
    float colorLut[4];   // VulkanLutStore::pushValues
};
static_assert(sizeof(ReframePush) == 128, "ReframePush must match the compute push_constant block");

// Push constants for fisheye_view_nv12.comp: 4 ints + 8 words, then the
// stab mat3 as padded columns (stab[3] = Natural) and the grade -- the same
// tail as ReframePush.
struct ViewPush {
    int32_t pass;
    int32_t outW;
    int32_t outH;
    int32_t fmt; // 0 = NV12, 1 = RGBA, 2 = P010
    float yaw;
    float pitch;
    float roll;
    float fovY;
    float aspect;
    int32_t origin; // the view's top-left in a canvas of views: x | y << 16 (x % 4 == 0,
                    // y % 2 == 0) ...
    int32_t canvas; // ... and the canvas: width | height << 16; 0 = the view alone
    float distortion; // view::Pose::distortion
    float stab[12];
    float colorGrade[4];
    float colorLut[4];
};
static_assert(sizeof(ViewPush) == 128, "ViewPush must match the compute push_constant block");

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
    // The graphics memory had no room for another slot at this many: the
    // ring makes do with them until memoryCapUntil, then tries to grow again.
    size_t memoryCap = 0;                              // guarded by mx
    std::chrono::steady_clock::time_point memoryCapUntil; // guarded by mx

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

// Ring sizing: enough for the playback queue plus what a viewport holds (the
// shown frame and its frames in flight). Past kLensSlotsSoft a fill waits for
// a slot to come back; past kLensSlotsHard it fails instead of growing. A
// slot the graphics memory has no room for is waited for the same way, from
// the slots there are (LensRingCore::memoryCap), for kLensMemoryRetry before
// the ring tries to grow again.
constexpr size_t kLensSlotsSoft = 10;
constexpr size_t kLensSlotsHard = 16;
constexpr auto kLensSlotWait = std::chrono::milliseconds(250);
constexpr auto kLensMemoryRetry = std::chrono::seconds(1);

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
    VkSamplerYcbcrConversion ycbcrConv = VK_NULL_HANDLE; // nv12 mode only
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    // The grade's LUTs (set 1 of the reproject and view pipelines).
    VulkanLutStore luts;

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
    // Submits `c`, which reads both lens frames, once the decoder is done with
    // them -- it waits on each frame's timeline semaphore at its value and
    // signals it one higher -- and waits for it to finish. *r0 / *r1 (if
    // given) get the values signalled and `newLayout`, the layout `c` left the
    // images in, for the decoder. *submitted (if given): when it was
    // submitted.
    bool submitAfterLenses(VkCommandBuffer c, const VulkanLensInput& f0, const VulkanLensInput& f1,
                           VkImageLayout newLayout, VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                           std::string& err,
                           std::chrono::steady_clock::time_point* submitted = nullptr)
    {
        const VkSemaphore sems[2] = {f0.semaphore, f1.semaphore};
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
        si.pWaitSemaphores = sems;
        si.pWaitDstStageMask = waitStages;
        si.signalSemaphoreCount = 2;
        si.pSignalSemaphores = sems;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        if (submitted)
            *submitted = std::chrono::steady_clock::now();
        if (submit(si) != VK_SUCCESS) {
            err = "vkQueueSubmit failed";
            return false;
        }
        if (r0)
            *r0 = {signalVals[0], newLayout};
        if (r1)
            *r1 = {signalVals[1], newLayout};
        vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
        vkResetFences(device, 1, &fence);
        return true;
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
    // The decoder's 2-plane format, the ycbcr conversion's: NV12, or P010
    // for 10-bit (initAdopt). Every image it hands over must have it.
    VkFormat lensFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    bool deepLens() const { return lensFormat != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; }
    // The decoder's stream's matrix and range (initAdopt).
    insv::YcbcrCoding ycbcr;
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
    // Depth-aware seam measures (binding 5): the cost volume, (NCC, texture)
    // per bin x candidate, then the cut statistics, four vec4 per bin x row.
    static constexpr VkDeviceSize kSeamCostBytes =
        sizeof(float) * 2 * insv::kSeamDepthBins * insv::kSeamDepthCands;
    static constexpr VkDeviceSize kSeamBufBytes =
        kSeamCostBytes + sizeof(float) * 16 * insv::kSeamDepthBins * insv::kSeamCutRows;
    VkBuffer seamBuf = VK_NULL_HANDLE;
    VkDeviceMemory seamMem = VK_NULL_HANDLE;
    void* seamMap = nullptr;
    // The new stitcher's seam flow (binding 6): the frame's field as an
    // RGBA16F texture of its grid (1 x 1 of zeros until a field comes),
    // wrapping round in azimuth, clamped across, and under the grid's rows
    // one more: the fade's share per column in R (insv::SeamFlowField::fade;
    // the GPU's own written by recordSeamFade). A frame whose field isn't
    // the one up (flowHeld) has it staged (ensureFlow) and copied in at the
    // start of its submission (recordFlowUpload). The image stays in
    // GENERAL from its first use on: seam_flow.comp binds it as a storage
    // image in every pass, so validation checks that layout at each
    // dispatch, though only the averaging pass writes it. The texture is
    // small; sampling it in GENERAL costs nothing measurable.
    VkSampler flowSampler = VK_NULL_HANDLE;
    VkImage flowImg = VK_NULL_HANDLE;
    VkDeviceMemory flowMem = VK_NULL_HANDLE;
    VkImageView flowView = VK_NULL_HANDLE;
    int flowW = 0, flowH = 0;
    VkBuffer flowStage = VK_NULL_HANDLE;
    VkDeviceMemory flowStageMem = VK_NULL_HANDLE;
    void* flowStageMap = nullptr;
    VkDeviceSize flowStageBytes = 0;
    std::shared_ptr<const insv::SeamFlowField> flowHeld;
    bool flowCopy = false;  // flowStage waits to be copied into flowImg
    bool flowFresh = false; // flowImg is new: its layout is still UNDEFINED, not GENERAL
    // The privacy blur's cell colours (sampled at binding 8, written by pass
    // 5 through binding 9): RGBA16F, a row of insv::kBlurCellsPerQuad per
    // quad. Made with the UBO (ensureUbo) and left in GENERAL from then on,
    // as flowImg.
    VkSampler blurCellSampler = VK_NULL_HANDLE; // nearest: texelFetch only
    VkImage blurCellImg = VK_NULL_HANDLE;
    VkDeviceMemory blurCellMem = VK_NULL_HANDLE;
    VkImageView blurCellView = VK_NULL_HANDLE;
    bool ensureBlurCells(std::string& err);
    // Pass 5 for p's quads into the begun `cmd` (the stitch pipeline, its set
    // bound to this frame's lenses), the table left readable by every later
    // stage. Nothing without quads.
    void recordBlurCells(VkCommandBuffer cmd, const insv::StitchGpuParams& p);
    // The seam flow's strips (binding 7, pass 4), device-local: the seam
    // flow (below) reads them.
    VkBuffer stripBuf = VK_NULL_HANDLE;
    VkDeviceMemory stripMem = VK_NULL_HANDLE;
    void* stripMap = nullptr;
    VkDeviceSize stripBytes = 0;
    // The seam flow on the GPU (VulkanSeamFlow.cpp, seam_flow.comp): its
    // pipeline (built when first used), per pyramid scale the shader's
    // scratch layout, the ring of the last frames' fields.
    struct SeamFlowScale {
        int W = 0, H = 0, f = 1, npx = 0, npy = 0;
        float sx = 1;
        int img = 0, grad = 0, patches = 0, dense = 0; // offsets into work, floats
    };
    struct {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkBuffer work = VK_NULL_HANDLE, ring = VK_NULL_HANDLE;
        VkDeviceMemory workMem = VK_NULL_HANDLE, ringMem = VK_NULL_HANDLE;
        int cols = 0, rows = 0;              // the grid the buffers are for
        float radX = 0, radY = 0;            // radians per strip pixel
        float reachRad = 0;                  // the grid's (readSeamFlow)
        std::vector<SeamFlowScale> scales;   // finest first
        int slot = 0;  // the ring slot the next frame goes into
        int count = 0; // frames held
        int anchor = 0;    // offset into work of the anchored strips (floats)
        // The fade (insv::SeamFadeFrame on the GPU): per ring slot both
        // lenses' patch means, fadeN patch centres from strip row fadeRow0;
        // the colour table it's solved with; per column the share, before
        // and after the spread (offsets into work, floats).
        int fadeMeans = 0, fadeColour = 0, fadeRaw = 0, fadeMax = 0;
        int fadeN = 0, fadeRow0 = 0;
    } sf;
    // The pipeline, the buffers for params' grid, the set pointed at the
    // strips and the texture (both made here if they aren't yet). Before
    // recording.
    bool ensureSeamFlow(const insv::StitchGpuParams& p, std::string& err);
    // The fade's share per column from the ring's patch means and p's seam
    // colour (insv::SeamFadeWindow::solve) into the texture's fade row,
    // where p stitches with the GPU's own field and its seam colour blend;
    // nothing otherwise. Before the passes that sample the texture.
    void recordSeamFade(VkCommandBuffer cmd, const insv::StitchGpuParams& p);
    // The flow of the strips in stripBuf added to the window and its mean
    // written into the flow texture, into the begun `cmd`; the texture left
    // readable by every later stage.
    void recordSeamFlow(VkCommandBuffer cmd);
    bool buildSeamFlowPipeline(std::string& err);
    // A one-off submission of what `rec` records into `cmd`, waited on (the
    // seam flow's tool and test calls).
    bool submitOnce(const std::function<void(VkCommandBuffer)>& rec, std::string& err);
    void destroySeamFlow();         // the buffers
    void destroySeamFlowPipeline(); // and the pipeline
    // The flow texture for a frame's params: (re)made at the field's size
    // (with seamFlowGpu, the grid's), the field staged when it isn't the
    // one up. Before recording.
    bool ensureFlow(const insv::StitchGpuParams& p, std::string& err);
    // The staged field copied into the texture, into the begun `cmd`; left
    // readable by every later stage. Nothing when nothing is staged.
    void recordFlowUpload(VkCommandBuffer cmd);
    // The strip buffer holding `bytes` (grows only), bound at 7 in both sets.
    bool ensureStrips(VkDeviceSize bytes, std::string& err);

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

    // The reproject and view passes' Natural builds (EmbeddedShaders.h), for a
    // view with Natural above 0: made the first time one is drawn. If one
    // can't be made it is said once and the view is drawn without Natural.
    VkPipeline rfPipelineNatural = VK_NULL_HANDLE;
    VkPipeline vwPipelineNatural = VK_NULL_HANDLE;
    bool rfNaturalFailed = false, vwNaturalFailed = false;
    // The pipeline of the reproject (`reframe`) or view pass for a view with
    // Natural `panini`.
    VkPipeline viewPassPipeline(bool reframe, float panini);

    // (Re)allocate the host-visible flat output buffer to hold `bytes`. Grows
    // only, so switching NV12 <-> RGBA or sizes doesn't thrash.
    bool ensureFlat(VkDeviceSize bytes, std::string& err);

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
                            VkDeviceMemory& mem, void** map);

    bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mprops,
                    VkBuffer& buf, VkDeviceMemory& mem, void** map);

    void destroyResources();

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
    void recordStitchPasses(int eqW, int eqH);

    // nv12 mode: a cached ycbcr-conversion view over one of the decoder's
    // pooled 2-plane images.
    VkImageView nv12ViewFor(VkImage img, VkFormat fmt);

    // UBO + gain buffer + seam cost buffer, created once, and bound (0, 4
    // and 5) in both the stitch set and the view set.
    bool ensureUbo(std::string& err);

    // The equirect output SSBO (stitch set binding 3), plus ensureUbo(). Both
    // modes.
    bool ensureOutput(int new_, int neh, std::string& err);

    // One exportable image with its own dedicated memory, and the memory's FD.
    bool makeExportImage(VkFormat fmt, int w, int h, VkImage& img, VkDeviceMemory& mem,
                         uint64_t& size, int& fd, std::string& err);

    // A free ring slot for lensW x lensH lenses and a flowW x flowH seam flow
    // field, marked busy -- reusing one, allocating a new one, or waiting for
    // one to come back.
    LensRingCore::Slot* acquireLensSlot(int lensW, int lensH, int flowW, int flowH,
                                        std::string& err);

    bool ensureResources(int nfw, int nfh, int new_, int neh, std::string& err);
};

} // namespace render
