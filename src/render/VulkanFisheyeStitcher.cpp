#include "VulkanFisheyeStitcherImpl.h"

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

// The per-frame work: lens frames in (uploaded RGB, or the decoder's own
// images), the stitch and seam passes, and the reprojection to a
// flat view or the lens frames playback shows. The device, pipelines and
// buffers they run on are VulkanFisheyeStitcherSetup.cpp's.

namespace render {

namespace {

constexpr uint32_t kLocalSize = 8;

double msBetween(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

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
    sp.edge[2] = static_cast<float>(p.seamColourActive);
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
    sp.seamC[3] = static_cast<float>(insv::SeamCutRowMax(p.bandRad));
    std::memcpy(sp.seamQ, p.seamQ, sizeof sp.seamQ);
    for (int b = 0; b < insv::kSeamDepthBins; ++b) {
        sp.seamCF[b][0] = p.seamBlend.cut[b];
        sp.seamCF[b][1] = p.seamBlend.fade[b];
    }
    for (int c = 0; c < 3; ++c) {
        sp.lensCol[c] = p.lensColour[c] + p.seamBlend.colour[c];
        sp.lensColS[c] = p.lensColourSlope[c];
    }
    sp.lensCol[3] = sp.lensColS[3] = 0.0f;
    for (int b = 0; b < insv::kSeamDepthBins; ++b) {
        for (int c = 0; c < 3; ++c) {
            sp.lensLocal[b][c] = p.seamBlend.local[b][c];
            sp.lensVeil[b][c] = p.seamBlend.veil[b][c];
        }
        sp.lensLocal[b][3] = sp.lensVeil[b][3] = 0.0f;
    }
    for (int b = 0; b < insv::kSeamDepthBins; ++b) {
        for (int c = 0; c < 3; ++c)
            sp.seamCol[b][c] = p.seamColourActive ? p.seamColour[b][c] : 0.0f;
        sp.seamCol[b][3] = 0.0f;
    }
    // The field's own grid when there is one (the texture is its size),
    // else the strip pass's (and the GPU's own field's).
    const bool flow = p.seamFlow && !p.seamFlow->empty();
    const bool gpuFlow = !flow && p.seamFlowGpu && p.seamFlowCols > 0;
    sp.seamFlow[0] = flow || gpuFlow ? 1.0f : 0.0f;
    sp.seamFlow[1] = flow ? p.seamFlow->grid.stripRad : p.seamFlowStripRad;
    sp.seamFlow[2] = flow ? p.seamFlow->grid.reachRad : p.seamFlowReachRad;
    sp.seamFlow[3] = 0.0f;
    sp.seamFlowN[0] = flow ? p.seamFlow->grid.cols : p.seamFlowCols;
    sp.seamFlowN[1] = flow ? p.seamFlow->grid.rows : p.seamFlowRows;
    sp.seamFlowN[2] = sp.seamFlowN[3] = 0;
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

// BT.709 limited range from 8-bit RGB, as the views convert their pixels
// (fisheye_view_nv12.comp, reframe_reproject.comp).
float lumaY(float r, float g, float b) { return 16.0f + 0.182586f * r + 0.614231f * g + 0.062007f * b; }
float chromaU(float r, float g, float b) { return 128.0f - 0.100644f * r - 0.338572f * g + 0.439216f * b; }
float chromaV(float r, float g, float b) { return 128.0f + 0.439216f * r - 0.398942f * g - 0.040274f * b; }
uint32_t q8(float x) { return static_cast<uint32_t>(std::clamp(x + 0.5f, 0.0f, 255.0f)); }
uint32_t q10(float x) { return static_cast<uint32_t>(std::clamp(x * 4.0f + 0.5f, 0.0f, 1023.0f)) << 6; }

// Fill a w x h canvas of views in `buf` with `rgba` (R in the low byte)
// before the views are drawn into it: what no view covers keeps it. RGBA8
// as it is; NV12 / P010 as its Y and CbCr, each plane its own fill.
void recordCanvasFill(VkCommandBuffer cmd, VkBuffer buf, VulkanFisheyeStitcher::ViewFormat fmt,
                      int w, int h, uint32_t rgba)
{
    const VkDeviceSize pixels = static_cast<VkDeviceSize>(w) * h;
    if (fmt == VulkanFisheyeStitcher::ViewFormat::Rgba) {
        vkCmdFillBuffer(cmd, buf, 0, pixels * 4, rgba);
        return;
    }
    const float r = float(rgba & 0xFF), g = float((rgba >> 8) & 0xFF), b = float((rgba >> 16) & 0xFF);
    const float y = lumaY(r, g, b), u = chromaU(r, g, b), v = chromaV(r, g, b);
    if (fmt == VulkanFisheyeStitcher::ViewFormat::P010) {
        vkCmdFillBuffer(cmd, buf, 0, pixels * 2, q10(y) | (q10(y) << 16));
        vkCmdFillBuffer(cmd, buf, pixels * 2, pixels, q10(u) | (q10(v) << 16));
        return;
    }
    vkCmdFillBuffer(cmd, buf, 0, pixels, q8(y) * 0x01010101u);
    vkCmdFillBuffer(cmd, buf, pixels, pixels / 2, (q8(u) | (q8(v) << 8)) * 0x00010001u);
}

// Whether `views` fit a canvasW x canvasH canvas of `fmt`: inside it, x and
// w on multiples of 4, h -- and for NV12 / P010's 2 x 2 chroma, y -- even.
bool viewsFit(int canvasW, int canvasH, const std::vector<VulkanFisheyeStitcher::ViewRect>& views,
              VulkanFisheyeStitcher::ViewFormat fmt, std::string& err)
{
    if (canvasW <= 0 || canvasH <= 0 || (canvasW & 3) != 0 || (canvasH & 1) != 0 ||
        canvasW > 0xFFFF || canvasH > 0xFFFF || views.empty()) {
        err = "bad canvas (width a multiple of 4, height of 2, at least one view)";
        return false;
    }
    const bool yuv = fmt != VulkanFisheyeStitcher::ViewFormat::Rgba;
    for (const VulkanFisheyeStitcher::ViewRect& v : views)
        if (v.x < 0 || v.y < 0 || (v.x & 3) != 0 || (yuv && (v.y & 1) != 0) ||
            v.x + v.w > canvasW || v.y + v.h > canvasH) {
            err = "view outside the canvas (or x not a multiple of 4, y not even)";
            return false;
        }
    return true;
}

} // namespace

void VulkanFisheyeStitcher::Impl::recordStitchPasses(int eqW, int eqH)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &descSet, 0,
                            nullptr);
    const uint32_t gx = (static_cast<uint32_t>(eqW) + kLocalSize - 1) / kLocalSize;
    const uint32_t gy = (static_cast<uint32_t>(eqH) + kLocalSize - 1) / kLocalSize;
    const int32_t pass = 1;
    vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
    vkCmdDispatch(cmd, gx, gy, 1);
}

bool VulkanFisheyeStitcher::stitch(const uint8_t* rgb0, const uint8_t* rgb1,
                                   const insv::StitchGpuParams& params, int eqW, int eqH,
                                   std::vector<uint8_t>& outRgba, std::string& err,
                                   bool measureSeamFlow)
{
    if (eqW <= 0 || eqH <= 0) {
        err = "bad dimensions";
        return false;
    }
    return stitchRgbImpl(rgb0, rgb1, params, eqW, eqH, &outRgba, measureSeamFlow, err);
}

bool VulkanFisheyeStitcher::measureSeamFlow(const uint8_t* rgb0, const uint8_t* rgb1,
                                            const insv::StitchGpuParams& params, std::string& err)
{
    return stitchRgbImpl(rgb0, rgb1, params, 0, 0, nullptr, true, err);
}

bool VulkanFisheyeStitcher::stitchRgbImpl(const uint8_t* rgb0, const uint8_t* rgb1,
                                          const insv::StitchGpuParams& paramsIn, int eqW, int eqH,
                                          std::vector<uint8_t>* outRgba, bool measureFlow,
                                          std::string& err)
{
    if (!m_ready) {
        err = "stitcher not initialised";
        return false;
    }
    // Measured here, the frame stitches with the GPU's own field.
    insv::StitchGpuParams flowParams;
    if (measureFlow) {
        flowParams = paramsIn;
        flowParams.seamFlow.reset();
        flowParams.seamFlowGpu = true;
    }
    const insv::StitchGpuParams& params = measureFlow ? flowParams : paramsIn;
    const bool doStitch = eqW > 0 && outRgba;
    const int fwv = params.fisheyeW;
    const int fhv = params.fisheyeH;
    if (fwv <= 0 || fhv <= 0) {
        err = "bad dimensions";
        return false;
    }
    // A measurement alone keeps whatever equirect buffer is there.
    if (!d->ensureResources(fwv, fhv, doStitch ? eqW : std::max(d->ew, 16),
                            doStitch ? eqH : std::max(d->eh, 8), err))
        return false;
    if (measureFlow && !d->ensureSeamFlow(params, err))
        return false;

    const size_t oneRgba = static_cast<size_t>(fwv) * fhv * 4;
    expandRgbToRgba(rgb0, static_cast<uint8_t*>(d->stagingMap), fwv, fhv);
    expandRgbToRgba(rgb1, static_cast<uint8_t*>(d->stagingMap) + oneRgba, fwv, fhv);

    ShaderParams sp{};
    packParams(params, doStitch ? eqW : 16, doStitch ? eqH : 8, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    if (!d->ensureFlow(params, err))
        return false;
    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->recordFlowUpload(d->cmd);
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

    if (measureFlow) {
        // Pass 4, the strips, then the flow from them (as measureSeamImpl).
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeline);
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeLayout, 0, 1,
                                &d->descSet, 0, nullptr);
        const int32_t pass = 4;
        vkCmdPushConstants(d->cmd, d->pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
        const uint32_t quads = static_cast<uint32_t>(params.seamFlowCols / 4);
        vkCmdDispatch(d->cmd, (quads + kLocalSize - 1) / kLocalSize,
                      (static_cast<uint32_t>(params.seamFlowRows) + kLocalSize - 1) / kLocalSize, 1);
        d->recordSeamFlow(d->cmd);
    }

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    if (doStitch)
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

    if (doStitch) {
        const size_t outBytes = static_cast<size_t>(eqW) * eqH * 4;
        outRgba->resize(outBytes);
        std::memcpy(outRgba->data(), d->outMap, outBytes);
    }
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

    if (!d->ensureFlow(params, err))
        return false;
    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->recordFlowUpload(d->cmd);
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
                                               int outW, int outH, const view::Pose* pose,
                                               const float stab[9], const color::Grade* colorGrade,
                                               std::vector<uint8_t>& outNv12, VulkanLensOutcome* r0,
                                               VulkanLensOutcome* r1, std::string& err)
{
    ViewRect v;
    if (pose)
        v.pose = *pose;
    v.w = outW;
    v.h = outH;
    return stitchAndReframeImpl(f0, f1, params, eqW, eqH, 0, 0, {v}, stab, colorGrade, nullptr,
                                outNv12, r0, r1, err);
}

bool VulkanFisheyeStitcher::stitchAndReframeViewsVk(
    const VulkanLensInput& f0, const VulkanLensInput& f1, const insv::StitchGpuParams& params,
    int eqW, int eqH, int canvasW, int canvasH, const std::vector<ViewRect>& views,
    const float stab[9], const color::Grade* colorGrade, uint32_t fillRgba,
    std::vector<uint8_t>& outNv12, VulkanLensOutcome* r0, VulkanLensOutcome* r1, std::string& err)
{
    if (!viewsFit(canvasW, canvasH, views, ViewFormat::Nv12, err))
        return false;
    return stitchAndReframeImpl(f0, f1, params, eqW, eqH, canvasW, canvasH, views, stab,
                                colorGrade, &fillRgba, outNv12, r0, r1, err);
}

bool VulkanFisheyeStitcher::stitchAndReframeImpl(
    const VulkanLensInput& f0, const VulkanLensInput& f1, const insv::StitchGpuParams& params,
    int eqW, int eqH, int canvasW, int canvasH, const std::vector<ViewRect>& views,
    const float stab[9], const color::Grade* colorGrade, const uint32_t* fill,
    std::vector<uint8_t>& outNv12, VulkanLensOutcome* r0, VulkanLensOutcome* r1, std::string& err)
{
    // A single view is its own canvas.
    const bool single = canvasW == 0;
    const int outW = single ? views[0].w : canvasW;
    const int outH = single ? views[0].h : canvasH;
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
    for (const ViewRect& v : views)
        if (v.w <= 0 || v.h <= 0 || (v.w & 3) != 0 || (v.h & 1) != 0) {
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

    // The LUT is uploaded (its own submission) before this frame is recorded.
    const color::Lut3D* lut = colorGrade && colorGrade->hasLut() ? colorGrade->lut.get() : nullptr;
    const VkDescriptorSet lutSet = d->luts.setFor(lut ? colorGrade->lut : nullptr);

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

    if (!d->ensureFlow(params, err))
        return false;
    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->recordFlowUpload(d->cmd);
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
    if (fill) { // the canvas background, where no view goes
        recordCanvasFill(d->cmd, d->flatBuf, ViewFormat::Nv12, outW, outH, *fill);
        VkBufferMemoryBarrier fb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        fb.srcQueueFamilyIndex = fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb.buffer = d->flatBuf;
        fb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &fb, 0,
                             nullptr);
    }

    ReframePush rp{};
    rp.eqW = eqW;
    rp.eqH = eqH;
    // Row-major stab -> col-major padded vec3 columns; nullptr => identity.
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            rp.stab[col * 4 + row] = stab ? stab[row * 3 + col] : (row == col ? 1.0f : 0.0f);
    for (int i = 0; i < 4; ++i)
        rp.colorGrade[i] = colorGrade ? colorGrade->adjust[i] : 0.0f;
    VulkanLutStore::pushValues(lut, colorGrade ? colorGrade->lutMix : 0.0f, rp.colorLut);

    vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->rfPipeline);
    const VkDescriptorSet rfSets[2] = {d->rfSet, lutSet};
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->rfPipeLayout, 0, 2, rfSets,
                            0, nullptr);
    for (const ViewRect& v : views) { // each view, 4 pixels per invocation
        rp.outW = v.w;
        rp.outH = v.h;
        rp.yaw = v.pose.yaw;
        rp.pitch = v.pose.pitch;
        rp.roll = v.pose.roll;
        const view::Lens lens = view::lens(v.pose, double(v.w) / double(v.h));
        rp.fovY = lens.fovY;
        rp.aspect = static_cast<float>(v.w) / static_cast<float>(v.h);
        rp.origin = single ? 0 : v.x | (v.y << 16);
        rp.distortion = lens.distortion;
        rp.canvas = single ? 0 : canvasW | (canvasH << 16);
        vkCmdPushConstants(d->cmd, d->rfPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp),
                           &rp);
        vkCmdDispatch(d->cmd, (static_cast<uint32_t>(v.w / 4) + 15) / 16,
                      (static_cast<uint32_t>(v.h) + 3) / 4, 1);
    }
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
                                         const insv::StitchGpuParams& params, int outW, int outH,
                                         const view::Pose* pose,
                                         const float stab[9], const color::Grade* colorGrade,
                                         ViewFormat fmt, std::vector<uint8_t>& out,
                                         VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                         std::string& err)
{
    ViewRect v;
    if (pose)
        v.pose = *pose;
    v.w = outW;
    v.h = outH;
    return renderViewsImpl(f0, f1, params, 0, 0, {v}, stab, colorGrade, fmt,
                           nullptr, out, r0, r1, err);
}

bool VulkanFisheyeStitcher::renderViewsVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                          const insv::StitchGpuParams& params, int canvasW, int canvasH,
                                          const std::vector<ViewRect>& views, const float stab[9],
                                          uint32_t clearRgba, std::vector<uint8_t>& out,
                                          VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                          std::string& err)
{
    return renderViewsVk(f0, f1, params, canvasW, canvasH, views, stab, nullptr,
                         ViewFormat::Rgba, clearRgba, out, r0, r1, err);
}

bool VulkanFisheyeStitcher::renderViewsVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                          const insv::StitchGpuParams& params, int canvasW, int canvasH,
                                          const std::vector<ViewRect>& views, const float stab[9],
                                          const color::Grade* colorGrade, ViewFormat fmt,
                                          uint32_t fillRgba, std::vector<uint8_t>& out,
                                          VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                          std::string& err)
{
    if (!viewsFit(canvasW, canvasH, views, fmt, err))
        return false;
    return renderViewsImpl(f0, f1, params, canvasW, canvasH, views, stab,
                           colorGrade, fmt, &fillRgba, out, r0, r1, err);
}

bool VulkanFisheyeStitcher::renderViewsImpl(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                            const insv::StitchGpuParams& params, int canvasW, int canvasH,
                                            const std::vector<ViewRect>& views,
                                            const float stab[9], const color::Grade* colorGrade,
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
    if (outW <= 0 || outH <= 0) {
        err = "bad dimensions (outW must be a multiple of 4, outH a multiple of 2)";
        return false;
    }
    const VkDeviceSize pixels = static_cast<VkDeviceSize>(outW) * outH;
    const VkDeviceSize outBytes = fmt == ViewFormat::Rgba   ? pixels * 4
                                  : fmt == ViewFormat::P010 ? pixels * 3
                                                            : pixels * 3 / 2;
    if (!d->ensureUbo(err) || !d->ensureFlat(outBytes, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    // The LUT is uploaded (its own submission) before this frame is recorded.
    const color::Lut3D* lut = colorGrade && colorGrade->hasLut() ? colorGrade->lut.get() : nullptr;
    const VkDescriptorSet lutSet = d->luts.setFor(lut ? colorGrade->lut : nullptr);

    ShaderParams sp{};
    packParams(params, 0, 0, sp); // no equirect: dims.zw unused
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

    if (!d->ensureFlow(params, err))
        return false;
    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->recordFlowUpload(d->cmd);
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

    if (clear) { // the canvas background, where no view goes
        recordCanvasFill(d->cmd, d->flatBuf, fmt, outW, outH, *clear);
        VkBufferMemoryBarrier fb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        fb.srcQueueFamilyIndex = fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb.buffer = d->flatBuf;
        fb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &fb, 0,
                             nullptr);
    }

    vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeline);
    const VkDescriptorSet vwSets[2] = {d->vwSet, lutSet};
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeLayout, 0, 2, vwSets,
                            0, nullptr);

    ViewPush vp{};
    vp.outW = outW;
    vp.outH = outH;
    vp.fmt = fmt == ViewFormat::Rgba ? 1 : fmt == ViewFormat::P010 ? 2 : 0;
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            vp.stab[col * 4 + row] = stab ? stab[row * 3 + col] : (row == col ? 1.0f : 0.0f);
    for (int i = 0; i < 4; ++i)
        vp.colorGrade[i] = colorGrade ? colorGrade->adjust[i] : 0.0f;
    VulkanLutStore::pushValues(lut, colorGrade ? colorGrade->lutMix : 0.0f, vp.colorLut);

    // Pass 1: each view, 4 pixels per invocation.
    vp.pass = 1;
    for (const ViewRect& v : views) {
        vp.outW = v.w;
        vp.outH = v.h;
        vp.yaw = v.pose.yaw;
        vp.pitch = v.pose.pitch;
        vp.roll = v.pose.roll;
        const view::Lens lens = view::lens(v.pose, double(v.w) / double(v.h));
        vp.fovY = lens.fovY;
        vp.aspect = static_cast<float>(v.w) / static_cast<float>(v.h);
        vp.origin = single ? 0 : v.x | (v.y << 16);
        vp.distortion = lens.distortion;
        vp.canvas = single ? 0 : canvasW | (canvasH << 16);
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
                                          insv::SeamCosts& out, insv::SeamCutStats* cutOut,
                                          VulkanLensOutcome* r0,
                                          VulkanLensOutcome* r1, std::string& err)
{
    return measureSeamImpl(f0, f1, params, &out, cutOut, false, r0, r1, err);
}

bool VulkanFisheyeStitcher::measureSeamCutVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                             const insv::StitchGpuParams& params,
                                             insv::SeamCutStats& out, VulkanLensOutcome* r0,
                                             VulkanLensOutcome* r1, std::string& err)
{
    return measureSeamImpl(f0, f1, params, nullptr, &out, false, r0, r1, err);
}

bool VulkanFisheyeStitcher::measureSeamFlowVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                              const insv::StitchGpuParams& params,
                                              insv::SeamCutStats* cutOut, VulkanLensOutcome* r0,
                                              VulkanLensOutcome* r1, std::string& err)
{
    return measureSeamImpl(f0, f1, params, nullptr, cutOut, true, r0, r1, err);
}

bool VulkanFisheyeStitcher::measureSeamImpl(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                            const insv::StitchGpuParams& params,
                                            insv::SeamCosts* out, insv::SeamCutStats* cutOut,
                                            bool flow, VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                            std::string& err)
{
    const int stripCols = params.seamFlowCols, stripRows = params.seamFlowRows;
    if (!m_ready || !d->nv12) {
        err = "measureSeamVk requires initAdopt()";
        return false;
    }
    // Pass 2 lives in both the stitch and the fused view shader. Prefer the
    // view pipeline: its binding 3 only needs a token buffer, where the
    // stitch's wants the whole equirect.
    const bool viaView = m_viewReady;
    if (flow && !d->ensureSeamFlow(params, err)) {
        err = "measureSeamFlowVk: " + err;
        return false;
    }
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
    if (!d->ensureFlow(params, err))
        return false;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    d->recordFlowUpload(cmd);

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
        const VkDescriptorSet sets[2] = {set, d->luts.setFor(nullptr)}; // no grade here
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeLayout, 0, 2, sets, 0,
                                nullptr);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeLayout, 0, 1, &set, 0,
                                nullptr);
    }
    if (out) {
        if (viaView) {
            ViewPush vp{};
            vp.pass = 2;
            vkCmdPushConstants(cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
        } else {
            const int32_t pass = 2;
            vkCmdPushConstants(cmd, d->pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
        }
        // One 64-invocation workgroup per (bin, candidate).
        vkCmdDispatch(cmd, insv::kSeamDepthBins, insv::kSeamDepthCands, 1);
    }
    // Pass 3, the cut statistics: one per (bin, row); writes its own part of
    // the buffer, so no barrier between the two.
    if (cutOut) {
        if (viaView) {
            ViewPush vp{};
            vp.pass = 3;
            vkCmdPushConstants(cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
        } else {
            const int32_t pass = 3;
            vkCmdPushConstants(cmd, d->pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
        }
        vkCmdDispatch(cmd, insv::kSeamDepthBins, insv::kSeamCutRows, 1);
    }
    // Pass 4, the seam flow's strips: four columns of a row per invocation,
    // into its own buffer; then the flow from them.
    if (flow) {
        const uint32_t quads = static_cast<uint32_t>(stripCols / 4);
        if (viaView) {
            ViewPush vp{};
            vp.pass = 4;
            vkCmdPushConstants(cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
            vkCmdDispatch(cmd, (quads + 15) / 16, (static_cast<uint32_t>(stripRows) + 3) / 4, 1);
        } else {
            const int32_t pass = 4;
            vkCmdPushConstants(cmd, d->pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
            vkCmdDispatch(cmd, (quads + kLocalSize - 1) / kLocalSize,
                          (static_cast<uint32_t>(stripRows) + kLocalSize - 1) / kLocalSize, 1);
        }
        d->recordSeamFlow(cmd);
    }

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
    if (out)
        for (int b = 0; b < insv::kSeamDepthBins; ++b)
            for (int k = 0; k < insv::kSeamDepthCands; ++k) {
                const float* v = c + 2 * (b * insv::kSeamDepthCands + k);
                out->ncc[b][k] = v[0];
                out->texture[b][k] = v[1];
            }
    if (cutOut) {
        const float* s = c + d->kSeamCostBytes / sizeof(float);
        for (int b = 0; b < insv::kSeamDepthBins; ++b)
            for (int r = 0; r < insv::kSeamCutRows; ++r) {
                const float* v = s + 16 * (b * insv::kSeamCutRows + r);
                insv::SeamCutStats::Cell& cell = cutOut->cell[b][r];
                cell.n = v[0];
                cell.zsad = v[1];
                cell.texture = v[2];
                for (int k = 0; k < 3; ++k) {
                    cell.nColour[k] = v[4 + k];
                    cell.mean[0][k] = v[8 + k];
                    cell.mean[1][k] = v[12 + k];
                }
            }
    }
    return true;
}

bool VulkanFisheyeStitcher::fillLensFrameVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                            const insv::StitchGpuParams& params, LensFrame& out,
                                            VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                            std::string& err)
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
    if (lw <= 0 || lh <= 0 || f1.width != lw || f1.height != lh || (lw & 1) || (lh & 1)) {
        err = "bad lens dimensions";
        return false;
    }

    if (!d->ensureFlow(params, err)) // the flow image's size first: the slot's
        return false;
    LensRingCore::Slot* slot = d->acquireLensSlot(lw, lh, d->flowW, d->flowH, err);
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
    packParams(params, 0, 0, sp); // no equirect: dims.zw unused
    // The viewport converts the planes itself (fisheye_view.frag).
    insv::YcbcrToRgbRows(d->ycbcr, d->deepLens() ? 10 : 8, sp.ycc);

    VkCommandBuffer cmd = d->cmd;
    if (!d->ensureFlow(params, err))
        return false;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    d->recordFlowUpload(cmd);
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

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);

    // Decoded pair -> transfer source (its writes are covered by the
    // semaphore wait); the slot's images -> transfer target (old contents
    // discarded, so no ownership acquire is needed).
    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier pre[2 + LensSlot::kImages];
    for (int i = 0; i < 2; ++i)
        pre[i] = imgBarrier(in[i]->image, in[i]->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                            VK_ACCESS_TRANSFER_READ_BIT);
    for (int i = 0; i < LensSlot::kImages; ++i)
        pre[2 + i] = imgBarrier(slot->img[i], VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 2 + LensSlot::kImages, pre);

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

    // The seam flow field, as this frame's params have it (recordFlowUpload
    // above left it readable by transfers).
    {
        VkImageMemoryBarrier fb = imgBarrier(d->flowImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                                             VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &fb);
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {static_cast<uint32_t>(d->flowW), static_cast<uint32_t>(d->flowH), 1};
        vkCmdCopyImage(cmd, d->flowImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       slot->img[LensSlot::kFlowImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        fb = imgBarrier(d->flowImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, 0);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &fb);
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

    out.fill = ++slot->fill;
    std::memcpy(out.params.data(), &sp, sizeof(sp));
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
