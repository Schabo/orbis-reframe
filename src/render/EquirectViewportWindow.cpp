// Before any Qt header: QVulkanInstance defines VK_NO_PROTOTYPES, and the
// renderer calls the Vulkan loader directly -- Qt's QVulkanDeviceFunctions
// are private ABI in Debian's Qt, which would pin the .deb to one Qt release.
#include <vulkan/vulkan.h>

#include "EquirectViewportWindow.h"

#include "../core/Log.h"
#include "EmbeddedShaders.h"
#include "FrameStats.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QWheelEvent>

#include <unistd.h> // dup / close: imported memory FDs

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace {

constexpr float kMinFovY = 0.35f;                 // ~20 degrees
constexpr float kMaxFovY = 1.92f;                 // ~110 degrees
constexpr float kMaxPitch = 1.55334f;             // ~89 degrees
constexpr float kMouseSensitivity = 0.005f;        // radians per pixel dragged
constexpr float kWheelFovStep = 0.05f;             // radians per wheel notch

struct PushConstants {
    float yaw;
    float pitch;
    float roll;
    float fovY;
    float aspect;
    float _pad[3];       // std140: mat3 below aligns at offset 32
    float stab[12];      // 3 columns, each a vec3 padded to 16 bytes (col-major)
    float colorGrade[4]; // exposureEv, contrast%, saturation%, temperature%
};

// fisheye_view.frag's push constants: the same view block plus the lens gains.
struct LensPushConstants {
    PushConstants view;
    float gains[4]; // x, y: photometric gain of lens 0 / 1
};
static_assert(sizeof(LensPushConstants) == 112, "LensPushConstants must match fisheye_view.frag");

// inset.frag's push constants.
struct InsetPushConstants {
    float rect[4];   // x, y, w, h in output-rect px
    float origin[2]; // output rect's top-left, framebuffer px
    float radius;
    float border;
    float borderColor[4]; // rgb, a = opacity
};
static_assert(sizeof(InsetPushConstants) == 48, "InsetPushConstants must match inset.frag");

// marker.frag's push constants (the Track subject box).
struct MarkerPushConstants {
    float rect[4];   // x, y, w, h in output-rect px
    float origin[2]; // output rect's top-left, framebuffer px
    float border;
    float fill;
    float color[4]; // rgb, a = outline opacity
};
static_assert(sizeof(MarkerPushConstants) == 48, "MarkerPushConstants must match marker.frag");

// region.frag's push constants (the privacy-blur region outlines).
struct RegionPushConstants {
    float origin[2]; // output rect's top-left, framebuffer px
    float border;
    float count;     // corners in use
    float color[4];  // rgb, a = opacity
    float pts[12];   // up to 6 corners in output-rect px
};
static_assert(sizeof(RegionPushConstants) == 80, "RegionPushConstants must match region.frag");

// A region outline as the view shows it: its quad's corners taken from the
// sphere frame into the view (the inverse of equirect.frag's ray: stab^T,
// then yaw, pitch and roll undone), clipped to what lies in front of the
// camera and projected into the output rect, px. Up to 6 corners (4, plus
// one per edge the clip cuts). Fewer than 3 = nothing to draw.
int projectOutline(const EquirectViewportWindow::RegionOutline &o, const float stabRm[9],
                   float yaw, float pitch, float roll, float fovY, float aspect, float rectW,
                   float rectH, float out[12])
{
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    float v[4][3];
    for (int i = 0; i < 4; ++i) {
        const float *d = o.corner[i];
        // stab^T * d (stab is a rotation; rayWorld = stab * rayPosed).
        float x = stabRm[0] * d[0] + stabRm[3] * d[1] + stabRm[6] * d[2];
        float y = stabRm[1] * d[0] + stabRm[4] * d[1] + stabRm[7] * d[2];
        float z = stabRm[2] * d[0] + stabRm[5] * d[1] + stabRm[8] * d[2];
        // Undo the yaw (about Y), the pitch (about X), the roll (about Z).
        float x1 = x * cy - z * sy, z1 = x * sy + z * cy;
        float y2 = y * cp + z1 * sp, z2 = -y * sp + z1 * cp;
        v[i][0] = x1 * cr + y2 * sr;
        v[i][1] = -x1 * sr + y2 * cr;
        v[i][2] = z2;
    }
    // Keep z <= -kNear (in front); Sutherland-Hodgman against that plane.
    constexpr float kNear = 1e-3f;
    float c[6][3];
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        const float *a = v[i], *b = v[(i + 1) % 4];
        const bool ina = a[2] <= -kNear, inb = b[2] <= -kNear;
        if (ina)
            std::copy(a, a + 3, c[n++]);
        if (ina != inb) {
            const float t = (-kNear - a[2]) / (b[2] - a[2]);
            for (int k = 0; k < 3; ++k)
                c[n][k] = a[k] + t * (b[k] - a[k]);
            ++n;
        }
    }
    const float t = std::tan(fovY * 0.5f);
    for (int i = 0; i < n; ++i) {
        const float ndcX = (c[i][0] / -c[i][2]) / (t * aspect);
        const float ndcY = -(c[i][1] / -c[i][2]) / t;
        out[i * 2] = (ndcX + 1.0f) * 0.5f * rectW;
        out[i * 2 + 1] = (ndcY + 1.0f) * 0.5f * rectH;
    }
    return n;
}

constexpr const char *kExternalMemoryFdExt = "VK_KHR_external_memory_fd";

// Rendering-only concern: a pipeline that samples an equirect texture through a
// fullscreen-triangle reprojection shader. See EquirectViewportWindow.h for why
// frames are pushed in from outside rather than pulled from an IStitchSource
// here.
class EquirectViewportRenderer : public QVulkanWindowRenderer {
public:
    explicit EquirectViewportRenderer(EquirectViewportWindow *window)
        : m_window(window)
    {
    }

    void initResources() override;
    void releaseResources() override;
    void startNextFrame() override;

private:
    VkShaderModule loadShader(render::shaders::Spirv spirv) const;
    void createSampler();
    void createDescriptorSet();
    void createPipeline();
    // (Re)creates the sampled image + staging ring when the frame size changes
    // (or on the first call), and points the descriptor set at the new view.
    void ensureTextureResources(uint32_t width, uint32_t height);
    void destroyTextureResources();
    // Records the barrier + buffer->image copy for staging slot `stagingSlot`
    // (already filled by the caller) into `cmd`. No submit, no wait.
    void recordTextureCopy(VkCommandBuffer cmd, uint32_t stagingSlot);
    // Blocking single upload for initResources only (placeholder / retained
    // frame restore) -- uses a one-time command buffer + vkQueueWaitIdle.
    void uploadTextureImmediate(int width, int height, const std::vector<uint8_t> &rgba);
    VkCommandBuffer beginOneTimeCommands() const;
    void endOneTimeCommands(VkCommandBuffer cmd) const;
    // A fullscreen-triangle graphics pipeline with equirect.vert and `frag`.
    // `blend`: straight alpha blending over what's drawn (the insets).
    VkPipeline makePipeline(render::shaders::Spirv frag, VkPipelineLayout layout, bool blend = false);

    // --- Picture-in-picture insets (inset.frag) ---
    struct InsetSlot {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
        bool undefined = true;
        // One staging buffer per frame in flight, like the main texture.
        std::vector<VkBuffer> staging;
        std::vector<VkDeviceMemory> stagingMem;
        std::vector<void *> stagingMap;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t key = 0; // frameKey of the picture in `img`
    };
    void createInsetResources();
    void destroyInsetResources();
    // (Re)size slot `i` for a w x h picture; waits for the device if it had
    // to recreate it.
    void ensureInsetSlot(InsetSlot &slot, uint32_t w, uint32_t h);
    void destroyInsetSlot(InsetSlot &slot);
    // Upload `rgba` into the slot's image, recorded into `cmd`.
    void recordInsetUpload(VkCommandBuffer cmd, InsetSlot &slot, const std::vector<uint8_t> &rgba);

    // --- Drawing lens frames (fisheye_view.frag) ---
    struct LensImport {
        VkImage img[render::LensSlot::kImages] = {};
        VkDeviceMemory mem[render::LensSlot::kImages] = {};
        VkImageView view[render::LensSlot::kImages] = {};
        uint64_t acquiredFill = 0; // fill last acquired from the exporter
        uint64_t lastUsed = 0;     // m_frameCounter
    };
    // Pipeline, layouts, per-frame UBOs / sets; decides m_lensCapable and
    // tells the window. Leaves the lens path off (not fatal) on any failure.
    void createLensResources();
    void destroyLensResources();
    // The (cached) import of `f`'s slot on this device; nullptr if it can't be
    // imported.
    LensImport *importFor(const render::LensFrame &f);
    void destroyImport(LensImport &imp);
    // Hand `f` back once every GPU frame that may use it has finished.
    void retireLens(std::shared_ptr<const render::LensFrame> f);

    EquirectViewportWindow *m_window;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkImage m_textureImage = VK_NULL_HANDLE;
    VkDeviceMemory m_textureMemory = VK_NULL_HANDLE;
    VkImageView m_textureView = VK_NULL_HANDLE;
    uint32_t m_texWidth = 0;
    uint32_t m_texHeight = 0;
    // True when m_textureImage has been (re)created but not written yet, so the
    // next upload transitions it from UNDEFINED rather than SHADER_READ_ONLY.
    bool m_textureUndefined = true;

    // Persistently-mapped host-visible staging buffers, one per frame in
    // flight. A per-frame texture upload is then just a memcpy into the current
    // slot + a copy recorded into that frame's own command buffer -- no
    // per-frame allocation and no vkQueueWaitIdle. Recreated (with the image)
    // only when the frame size changes.
    std::vector<VkBuffer> m_stagingBuffers;
    std::vector<VkDeviceMemory> m_stagingMemory;
    std::vector<void *> m_stagingMapped;

    // Last frame uploaded, retained so it can be re-uploaded if Vulkan
    // resources are torn down and rebuilt (Qt does this on some resize /
    // surface-recreation paths, notably under Wayland). Without this, a
    // paused frame vanishes on resize -- nothing re-pushes it, since the
    // playback clock only calls SetFrame() while playing or just after a seek.
    int m_lastWidth = 0;
    int m_lastHeight = 0;
    std::vector<uint8_t> m_lastRgba;

    // --- lens frames ---
    bool m_lensCapable = false;
    uint8_t m_deviceUuid[16] = {};
    VkPhysicalDeviceMemoryProperties m_memProps{};
    VkDescriptorSetLayout m_lensSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_lensPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_lensSets; // one per frame in flight
    std::vector<VkBuffer> m_lensUbo;         // the stitch Params, one per frame in flight
    std::vector<VkDeviceMemory> m_lensUboMem;
    std::vector<void *> m_lensUboMap;
    VkPipelineLayout m_lensPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_lensPipeline = VK_NULL_HANDLE;
    VkSampler m_lensSampler = VK_NULL_HANDLE;
    std::map<uint64_t, LensImport> m_imports; // by LensSlot::id
    // What's on screen in lens mode. Survives releaseResources (like
    // m_lastRgba) so a rebuilt device re-imports it.
    std::shared_ptr<const render::LensFrame> m_currentLens;
    bool m_lensMode = false;
    struct Retired {
        std::shared_ptr<const render::LensFrame> frame;
        uint64_t releaseAt; // m_frameCounter at which no frame in flight uses it
    };
    std::deque<Retired> m_retired;
    uint64_t m_frameCounter = 0;

    // ORBIS_FRAME_STATS (FrameStats.h): render pacing, and for how many
    // renders each new picture stayed on screen.
    struct Stats {
        render::framestats::Clock::time_point windowStart{}, lastRender{};
        render::framestats::Samples interval, cpu;
        int newFrames = 0;
        int rendersOfCurrent = 0;
        int shownFor[5] = {}; // 1, 2, 3, 4, 5+ renders
        int poseChanges = 0;
        float lastPose[4] = {};
    } m_stats;
    void statsFrameStart(bool newPicture);
    void statsFrameEnd(render::framestats::Clock::time_point start);

    // --- insets ---
    InsetSlot m_insetSlots[EquirectViewportWindow::kMaxInsets];
    VkSampler m_insetSampler = VK_NULL_HANDLE;
    VkDescriptorPool m_insetPool = VK_NULL_HANDLE;
    VkPipelineLayout m_insetPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_insetPipeline = VK_NULL_HANDLE;

    // The Track subject box (marker.frag): no descriptors, push constants only.
    VkPipelineLayout m_markerPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_markerPipeline = VK_NULL_HANDLE;
    // The privacy-blur region outlines (region.frag): push constants only.
    VkPipelineLayout m_regionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_regionPipeline = VK_NULL_HANDLE;
};

VkCommandBuffer EquirectViewportRenderer::beginOneTimeCommands() const
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_window->graphicsCommandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_window->device(), &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    return cmd;
}

void EquirectViewportRenderer::endOneTimeCommands(VkCommandBuffer cmd) const
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkQueue queue = m_window->graphicsQueue();
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    // A one-off upload (new frame arriving), not per-render-frame -- a full
    // wait-idle is simple and correct; revisit if/when uploads need to
    // happen every rendered frame for real playback.
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(m_window->device(), m_window->graphicsCommandPool(), 1, &cmd);
}

VkShaderModule EquirectViewportRenderer::loadShader(render::shaders::Spirv spirv) const
{
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.bytes;
    info.pCode = spirv.code;

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_window->device(), &info, nullptr, &module) != VK_SUCCESS) {
        qFatal("Failed to create shader module");
    }
    return module;
}

void EquirectViewportRenderer::createSampler()
{
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // U wraps around the equirect seam; V does not (poles are the top/bottom
    // edges of the texture, not a wraparound).
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    vkCreateSampler(m_window->device(), &info, nullptr, &m_sampler);
}

void EquirectViewportRenderer::createDescriptorSet()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(m_window->device(), &layoutInfo, nullptr,
                                             &m_descriptorSetLayout);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(m_window->device(), &poolInfo, nullptr, &m_descriptorPool);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    vkAllocateDescriptorSets(m_window->device(), &allocInfo, &m_descriptorSet);
}

VkPipeline EquirectViewportRenderer::makePipeline(render::shaders::Spirv frag, VkPipelineLayout layout,
                                                  bool blend)
{
    VkShaderModule vertShader = loadShader(render::shaders::equirectVert());
    VkShaderModule fragShader = loadShader(frag);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = m_window->sampleCountFlagBits();

    // QVulkanWindow's default render pass has a depth-stencil attachment; the
    // fullscreen triangle neither tests nor writes it, but the pipeline must
    // still say so (VUID-VkGraphicsPipelineCreateInfo-renderPass-09028).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend) {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    static const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = m_window->defaultRenderPass();

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(m_window->device(), VK_NULL_HANDLE, 1,
                                                             &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(m_window->device(), vertShader, nullptr);
    vkDestroyShaderModule(m_window->device(), fragShader, nullptr);
    return r == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

void EquirectViewportRenderer::createPipeline()
{
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    vkCreatePipelineLayout(m_window->device(), &layoutInfo, nullptr, &m_pipelineLayout);

    m_pipeline = makePipeline(render::shaders::equirectFrag(),
                              m_pipelineLayout);
    if (m_pipeline == VK_NULL_HANDLE)
        qFatal("Failed to create equirect graphics pipeline");
}

void EquirectViewportRenderer::createLensResources()
{
    m_lensCapable = false;
    m_window->setLensFramesSupported(false);
    QVulkanInstance *inst = m_window->vulkanInstance();
    VkDevice device = m_window->device();

    // Needs Vulkan 1.1 (external memory / dedicated allocation structs) and
    // FD import, which MainWindow asks QVulkanWindow to enable.
    if (inst->apiVersion() < QVersionNumber(1, 1) ||
        !m_window->supportedDeviceExtensions().contains(QByteArray(kExternalMemoryFdExt))) {
        std::fprintf(stderr, "[viewport] no external-memory FD import; playback stays on the "
                             "equirect preview\n");
        return;
    }
    auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        inst->getInstanceProcAddr("vkGetPhysicalDeviceProperties2"));
    if (!getProps2)
        return;
    VkPhysicalDeviceIDProperties idp{};
    idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &idp;
    getProps2(m_window->physicalDevice(), &p2);
    std::memcpy(m_deviceUuid, idp.deviceUUID, sizeof m_deviceUuid);
    vkGetPhysicalDeviceMemoryProperties(m_window->physicalDevice(), &m_memProps);

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(device, &si, nullptr, &m_lensSampler) != VK_SUCCESS)
        return;

    VkDescriptorSetLayoutBinding b[5]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 1; i <= 4; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 5;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(device, &sli, nullptr, &m_lensSetLayout) != VK_SUCCESS)
        return;

    const uint32_t frames = static_cast<uint32_t>(std::max(1, m_window->concurrentFrameCount()));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frames},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * frames}};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = frames;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(device, &dpi, nullptr, &m_lensPool) != VK_SUCCESS)
        return;
    m_lensSets.assign(frames, VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(frames, m_lensSetLayout);
    VkDescriptorSetAllocateInfo dsi{};
    dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsi.descriptorPool = m_lensPool;
    dsi.descriptorSetCount = frames;
    dsi.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &dsi, m_lensSets.data()) != VK_SUCCESS)
        return;

    m_lensUbo.assign(frames, VK_NULL_HANDLE);
    m_lensUboMem.assign(frames, VK_NULL_HANDLE);
    m_lensUboMap.assign(frames, nullptr);
    for (uint32_t i = 0; i < frames; ++i) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = render::LensFrame::kParamsBytes;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &m_lensUbo[i]) != VK_SUCCESS)
            return;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, m_lensUbo[i], &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = m_window->hostVisibleMemoryIndex();
        if (vkAllocateMemory(device, &ai, nullptr, &m_lensUboMem[i]) != VK_SUCCESS)
            return;
        vkBindBufferMemory(device, m_lensUbo[i], m_lensUboMem[i], 0);
        vkMapMemory(device, m_lensUboMem[i], 0, bi.size, 0, &m_lensUboMap[i]);
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(LensPushConstants)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_lensSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device, &pli, nullptr, &m_lensPipelineLayout) != VK_SUCCESS)
        return;
    m_lensPipeline = makePipeline(render::shaders::fisheyeViewFrag(),
                                  m_lensPipelineLayout);
    if (m_lensPipeline == VK_NULL_HANDLE)
        return;

    m_lensCapable = true;
    m_window->setLensFramesSupported(true);
}

void EquirectViewportRenderer::destroyImport(LensImport &imp)
{
    VkDevice device = m_window->device();
    for (int i = 0; i < render::LensSlot::kImages; ++i) {
        if (imp.view[i])
            vkDestroyImageView(device, imp.view[i], nullptr);
        if (imp.img[i])
            vkDestroyImage(device, imp.img[i], nullptr);
        if (imp.mem[i])
            vkFreeMemory(device, imp.mem[i], nullptr);
        imp.view[i] = VK_NULL_HANDLE;
        imp.img[i] = VK_NULL_HANDLE;
        imp.mem[i] = VK_NULL_HANDLE;
    }
}

void EquirectViewportRenderer::destroyLensResources()
{
    VkDevice device = m_window->device();
    for (auto &[id, imp] : m_imports)
        destroyImport(imp);
    m_imports.clear();
    m_retired.clear(); // the device is idle: every frame using them is done
    for (size_t i = 0; i < m_lensUbo.size(); ++i) {
        if (m_lensUboMap[i])
            vkUnmapMemory(device, m_lensUboMem[i]);
        if (m_lensUbo[i])
            vkDestroyBuffer(device, m_lensUbo[i], nullptr);
        if (m_lensUboMem[i])
            vkFreeMemory(device, m_lensUboMem[i], nullptr);
    }
    m_lensUbo.clear();
    m_lensUboMem.clear();
    m_lensUboMap.clear();
    m_lensSets.clear();
    if (m_lensPipeline)
        vkDestroyPipeline(device, m_lensPipeline, nullptr);
    if (m_lensPipelineLayout)
        vkDestroyPipelineLayout(device, m_lensPipelineLayout, nullptr);
    if (m_lensPool)
        vkDestroyDescriptorPool(device, m_lensPool, nullptr);
    if (m_lensSetLayout)
        vkDestroyDescriptorSetLayout(device, m_lensSetLayout, nullptr);
    if (m_lensSampler)
        vkDestroySampler(device, m_lensSampler, nullptr);
    m_lensPipeline = VK_NULL_HANDLE;
    m_lensPipelineLayout = VK_NULL_HANDLE;
    m_lensPool = VK_NULL_HANDLE;
    m_lensSetLayout = VK_NULL_HANDLE;
    m_lensSampler = VK_NULL_HANDLE;
    m_lensCapable = false;
    m_window->setLensFramesSupported(false);
}

EquirectViewportRenderer::LensImport *
EquirectViewportRenderer::importFor(const render::LensFrame &f)
{
    const render::LensSlot &slot = *f.slot;
    auto it = m_imports.find(slot.id);
    if (it != m_imports.end())
        return &it->second;
    if (std::memcmp(slot.deviceUuid, m_deviceUuid, sizeof m_deviceUuid) != 0) {
        std::fprintf(stderr, "[viewport] lens frames come from another GPU; can't import\n");
        return nullptr;
    }

    VkDevice device = m_window->device();
    LensImport imp;
    for (int i = 0; i < render::LensSlot::kImages; ++i) {
        const bool uv = (i & 1) != 0;
        const VkFormat fmt = uv ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM;
        // Must match the exporter's image exactly (VulkanFisheyeStitcher).
        VkExternalMemoryImageCreateInfo emi{};
        emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.pNext = &emi;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {static_cast<uint32_t>(uv ? slot.lensW / 2 : slot.lensW),
                     static_cast<uint32_t>(uv ? slot.lensH / 2 : slot.lensH), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool ok = vkCreateImage(device, &ii, nullptr, &imp.img[i]) == VK_SUCCESS;

        VkMemoryRequirements req{};
        uint32_t type = UINT32_MAX;
        if (ok) {
            vkGetImageMemoryRequirements(device, imp.img[i], &req);
            for (uint32_t t = 0; t < m_memProps.memoryTypeCount && type == UINT32_MAX; ++t)
                if ((req.memoryTypeBits & (1u << t)) &&
                    (m_memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    type = t;
            ok = type != UINT32_MAX;
        }
        if (ok) {
            const int fd = ::dup(slot.fd[i]); // the import takes ownership of this copy
            VkMemoryDedicatedAllocateInfo ded{};
            ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            ded.image = imp.img[i];
            VkImportMemoryFdInfoKHR imf{};
            imf.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
            imf.pNext = &ded;
            imf.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            imf.fd = fd;
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.pNext = &imf;
            ai.allocationSize = slot.memSize[i];
            ai.memoryTypeIndex = type;
            ok = fd >= 0 &&
                 vkAllocateMemory(device, &ai, nullptr, &imp.mem[i]) == VK_SUCCESS;
            if (!ok && fd >= 0)
                ::close(fd); // not consumed on failure
        }
        if (ok)
            ok = vkBindImageMemory(device, imp.img[i], imp.mem[i], 0) == VK_SUCCESS;
        if (ok) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = imp.img[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = fmt;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ok = vkCreateImageView(device, &vi, nullptr, &imp.view[i]) == VK_SUCCESS;
        }
        if (!ok) {
            std::fprintf(stderr, "[viewport] importing a lens frame failed; back to the "
                                 "equirect preview\n");
            destroyImport(imp);
            return nullptr;
        }
    }
    return &m_imports.emplace(slot.id, imp).first->second;
}

void EquirectViewportRenderer::retireLens(std::shared_ptr<const render::LensFrame> f)
{
    if (!f)
        return;
    // A frame recorded now finishes before the one concurrentFrameCount()
    // frames later starts (Qt waits on that frame slot's fence).
    const uint64_t frames = static_cast<uint64_t>(std::max(1, m_window->concurrentFrameCount()));
    m_retired.push_back({std::move(f), m_frameCounter + frames});
}

void EquirectViewportRenderer::ensureTextureResources(uint32_t width, uint32_t height)
{
    if (width == m_texWidth && height == m_texHeight && m_textureImage != VK_NULL_HANDLE)
        return;

    VkDevice device = m_window->device();

    // Only reached on the first upload and on a frame-size change (e.g. a clip
    // stitched at a different preview resolution) -- never the per-frame path.
    // Make sure no in-flight frame is still reading the old image or writing an
    // old staging buffer before tearing them down.
    if (m_textureImage != VK_NULL_HANDLE || !m_stagingBuffers.empty())
        vkDeviceWaitIdle(device);
    destroyTextureResources();

    // --- Device-local sampled image + view ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &imageInfo, nullptr, &m_textureImage);

        VkMemoryRequirements memReq{};
        vkGetImageMemoryRequirements(device, m_textureImage, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = m_window->deviceLocalMemoryIndex();
        vkAllocateMemory(device, &allocInfo, nullptr, &m_textureMemory);
        vkBindImageMemory(device, m_textureImage, m_textureMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_textureImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &viewInfo, nullptr, &m_textureView);
    }

    // --- Host-visible staging ring (one buffer per frame in flight) ---
    const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(width) * height * 4;
    const int frames = std::max(1, m_window->concurrentFrameCount());
    m_stagingBuffers.assign(frames, VK_NULL_HANDLE);
    m_stagingMemory.assign(frames, VK_NULL_HANDLE);
    m_stagingMapped.assign(frames, nullptr);
    for (int i = 0; i < frames; ++i) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufferInfo, nullptr, &m_stagingBuffers[i]);

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(device, m_stagingBuffers[i], &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = m_window->hostVisibleMemoryIndex();
        vkAllocateMemory(device, &allocInfo, nullptr, &m_stagingMemory[i]);
        vkBindBufferMemory(device, m_stagingBuffers[i], m_stagingMemory[i], 0);
        vkMapMemory(device, m_stagingMemory[i], 0, bufferSize, 0, &m_stagingMapped[i]);
    }

    // Point the persistent descriptor set at the new view. Safe without a
    // wait here: the only paths that reach this recreated the resources after
    // a vkDeviceWaitIdle (above) or on the very first upload, so no pending
    // command buffer references the old descriptor contents.
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_textureView;
    imageInfo.sampler = m_sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    m_texWidth = width;
    m_texHeight = height;
    m_textureUndefined = true;
}

void EquirectViewportRenderer::destroyTextureResources()
{
    VkDevice device = m_window->device();

    for (size_t i = 0; i < m_stagingBuffers.size(); ++i) {
        if (m_stagingMapped[i])
            vkUnmapMemory(device, m_stagingMemory[i]);
        if (m_stagingBuffers[i] != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_stagingBuffers[i], nullptr);
        if (m_stagingMemory[i] != VK_NULL_HANDLE)
            vkFreeMemory(device, m_stagingMemory[i], nullptr);
    }
    m_stagingBuffers.clear();
    m_stagingMemory.clear();
    m_stagingMapped.clear();

    if (m_textureView != VK_NULL_HANDLE)
        vkDestroyImageView(device, m_textureView, nullptr);
    if (m_textureImage != VK_NULL_HANDLE)
        vkDestroyImage(device, m_textureImage, nullptr);
    if (m_textureMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, m_textureMemory, nullptr);
    m_textureView = VK_NULL_HANDLE;
    m_textureImage = VK_NULL_HANDLE;
    m_textureMemory = VK_NULL_HANDLE;
    m_texWidth = 0;
    m_texHeight = 0;
    m_textureUndefined = true;
}

void EquirectViewportRenderer::recordTextureCopy(VkCommandBuffer cmd, uint32_t stagingSlot)
{
    VkImageMemoryBarrier toTransferDst{};
    toTransferDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransferDst.oldLayout = m_textureUndefined ? VK_IMAGE_LAYOUT_UNDEFINED
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDst.image = m_textureImage;
    toTransferDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransferDst.srcAccessMask = m_textureUndefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags srcStage = m_textureUndefined
        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
        : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                     0, nullptr, 1, &toTransferDst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_texWidth, m_texHeight, 1};
    vkCmdCopyBufferToImage(cmd, m_stagingBuffers[stagingSlot], m_textureImage,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = m_textureImage;
    toShaderRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toShaderRead);

    m_textureUndefined = false;
}

void EquirectViewportRenderer::uploadTextureImmediate(int width, int height,
                                                      const std::vector<uint8_t> &rgba)
{
    ensureTextureResources(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    std::memcpy(m_stagingMapped[0], rgba.data(), rgba.size());

    VkCommandBuffer cmd = beginOneTimeCommands();
    recordTextureCopy(cmd, 0);
    endOneTimeCommands(cmd); // waits for the queue to go idle -- acceptable on
                             // this one-off path (initResources only)
}

void EquirectViewportRenderer::createInsetResources()
{
    VkDevice device = m_window->device();
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    vkCreateSampler(device, &si, nullptr, &m_insetSampler);

    // One combined image sampler per slot -- the same set layout as the
    // equirect texture's.
    constexpr uint32_t n = EquirectViewportWindow::kMaxInsets;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = n;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    vkCreateDescriptorPool(device, &dpi, nullptr, &m_insetPool);
    VkDescriptorSetLayout layouts[n];
    std::fill(layouts, layouts + n, m_descriptorSetLayout);
    VkDescriptorSet sets[n]{};
    VkDescriptorSetAllocateInfo dsi{};
    dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsi.descriptorPool = m_insetPool;
    dsi.descriptorSetCount = n;
    dsi.pSetLayouts = layouts;
    vkAllocateDescriptorSets(device, &dsi, sets);
    for (uint32_t i = 0; i < n; ++i) {
        m_insetSlots[i].set = sets[i];
        m_insetSlots[i].key = 0;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(InsetPushConstants)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_descriptorSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(device, &pli, nullptr, &m_insetPipelineLayout);
    m_insetPipeline = makePipeline(render::shaders::insetFrag(),
                                   m_insetPipelineLayout, /*blend=*/true);
    if (m_insetPipeline == VK_NULL_HANDLE)
        std::fprintf(stderr, "[viewport] inset pipeline failed; insets won't show\n");

    VkPushConstantRange mpcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MarkerPushConstants)};
    VkPipelineLayoutCreateInfo mpli{};
    mpli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    mpli.pushConstantRangeCount = 1;
    mpli.pPushConstantRanges = &mpcr;
    vkCreatePipelineLayout(device, &mpli, nullptr, &m_markerPipelineLayout);
    m_markerPipeline = makePipeline(render::shaders::markerFrag(),
                                    m_markerPipelineLayout, /*blend=*/true);
    if (m_markerPipeline == VK_NULL_HANDLE)
        std::fprintf(stderr, "[viewport] marker pipeline failed; the tracking box won't show\n");

    VkPushConstantRange rpcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RegionPushConstants)};
    VkPipelineLayoutCreateInfo rpli{};
    rpli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    rpli.pushConstantRangeCount = 1;
    rpli.pPushConstantRanges = &rpcr;
    vkCreatePipelineLayout(device, &rpli, nullptr, &m_regionPipelineLayout);
    m_regionPipeline = makePipeline(render::shaders::regionFrag(),
                                    m_regionPipelineLayout, /*blend=*/true);
    if (m_regionPipeline == VK_NULL_HANDLE)
        std::fprintf(stderr, "[viewport] region pipeline failed; the blur regions won't show\n");
}

void EquirectViewportRenderer::destroyInsetSlot(InsetSlot &slot)
{
    VkDevice device = m_window->device();
    for (size_t i = 0; i < slot.staging.size(); ++i) {
        if (slot.stagingMap[i])
            vkUnmapMemory(device, slot.stagingMem[i]);
        vkDestroyBuffer(device, slot.staging[i], nullptr);
        vkFreeMemory(device, slot.stagingMem[i], nullptr);
    }
    slot.staging.clear();
    slot.stagingMem.clear();
    slot.stagingMap.clear();
    if (slot.view)
        vkDestroyImageView(device, slot.view, nullptr);
    if (slot.img)
        vkDestroyImage(device, slot.img, nullptr);
    if (slot.mem)
        vkFreeMemory(device, slot.mem, nullptr);
    slot.view = VK_NULL_HANDLE;
    slot.img = VK_NULL_HANDLE;
    slot.mem = VK_NULL_HANDLE;
    slot.w = slot.h = 0;
    slot.undefined = true;
    slot.key = 0;
}

void EquirectViewportRenderer::destroyInsetResources()
{
    VkDevice device = m_window->device();
    for (InsetSlot &slot : m_insetSlots) {
        destroyInsetSlot(slot);
        slot.set = VK_NULL_HANDLE; // freed with the pool
    }
    if (m_insetPipeline)
        vkDestroyPipeline(device, m_insetPipeline, nullptr);
    if (m_insetPipelineLayout)
        vkDestroyPipelineLayout(device, m_insetPipelineLayout, nullptr);
    if (m_insetPool)
        vkDestroyDescriptorPool(device, m_insetPool, nullptr);
    if (m_markerPipeline)
        vkDestroyPipeline(device, m_markerPipeline, nullptr);
    if (m_markerPipelineLayout)
        vkDestroyPipelineLayout(device, m_markerPipelineLayout, nullptr);
    m_markerPipeline = VK_NULL_HANDLE;
    m_markerPipelineLayout = VK_NULL_HANDLE;
    if (m_regionPipeline)
        vkDestroyPipeline(device, m_regionPipeline, nullptr);
    if (m_regionPipelineLayout)
        vkDestroyPipelineLayout(device, m_regionPipelineLayout, nullptr);
    m_regionPipeline = VK_NULL_HANDLE;
    m_regionPipelineLayout = VK_NULL_HANDLE;
    if (m_insetSampler)
        vkDestroySampler(device, m_insetSampler, nullptr);
    m_insetPipeline = VK_NULL_HANDLE;
    m_insetPipelineLayout = VK_NULL_HANDLE;
    m_insetPool = VK_NULL_HANDLE;
    m_insetSampler = VK_NULL_HANDLE;
}

void EquirectViewportRenderer::ensureInsetSlot(InsetSlot &slot, uint32_t w, uint32_t h)
{
    if (slot.img && slot.w == w && slot.h == h)
        return;
    VkDevice device = m_window->device();
    // A new clip size: rare (per clip, not per frame). Nothing in flight may
    // still read the old image or its descriptor.
    if (slot.img)
        vkDeviceWaitIdle(device);
    const VkDescriptorSet set = slot.set;
    destroyInsetSlot(slot);
    slot.set = set;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device, &ii, nullptr, &slot.img);
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, slot.img, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_window->deviceLocalMemoryIndex();
    vkAllocateMemory(device, &ai, nullptr, &slot.mem);
    vkBindImageMemory(device, slot.img, slot.mem, 0);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = slot.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &vi, nullptr, &slot.view);

    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    const int frames = std::max(1, m_window->concurrentFrameCount());
    slot.staging.assign(frames, VK_NULL_HANDLE);
    slot.stagingMem.assign(frames, VK_NULL_HANDLE);
    slot.stagingMap.assign(frames, nullptr);
    for (int i = 0; i < frames; ++i) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bi, nullptr, &slot.staging[i]);
        VkMemoryRequirements br{};
        vkGetBufferMemoryRequirements(device, slot.staging[i], &br);
        VkMemoryAllocateInfo bai{};
        bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize = br.size;
        bai.memoryTypeIndex = m_window->hostVisibleMemoryIndex();
        vkAllocateMemory(device, &bai, nullptr, &slot.stagingMem[i]);
        vkBindBufferMemory(device, slot.staging[i], slot.stagingMem[i], 0);
        vkMapMemory(device, slot.stagingMem[i], 0, bytes, 0, &slot.stagingMap[i]);
    }

    VkDescriptorImageInfo di{m_insetSampler, slot.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = slot.set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
    slot.w = w;
    slot.h = h;
    slot.undefined = true;
}

void EquirectViewportRenderer::recordInsetUpload(VkCommandBuffer cmd, InsetSlot &slot,
                                                 const std::vector<uint8_t> &rgba)
{
    const uint32_t frame = static_cast<uint32_t>(m_window->currentFrame());
    std::memcpy(slot.stagingMap[frame], rgba.data(), std::min<size_t>(rgba.size(), size_t(slot.w) * slot.h * 4));

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = slot.undefined ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = slot.img;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = slot.undefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                                     slot.undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                    : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &toDst);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {slot.w, slot.h, 1};
    vkCmdCopyBufferToImage(cmd, slot.staging[frame], slot.img,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);
    slot.undefined = false;
}

void EquirectViewportRenderer::initResources()
{
    VkDevice device = m_window->device();

    createSampler();
    createDescriptorSet();
    createPipeline();
    createLensResources();
    createInsetResources();

    if (!m_lastRgba.empty()) {
        // Resources were rebuilt (e.g. resize on Wayland) -- restore the frame
        // that was on screen rather than blanking to the placeholder.
        uploadTextureImmediate(m_lastWidth, m_lastHeight, m_lastRgba);
    } else {
        // Placeholder so the descriptor set is always valid, even before the
        // first real frame arrives.
        const std::vector<uint8_t> placeholder(4, 128);
        uploadTextureImmediate(1, 1, placeholder);
    }
    // Queued: listeners may open dialogs, which mustn't run inside Qt's
    // Vulkan initialization.
    QMetaObject::invokeMethod(m_window, &EquirectViewportWindow::vulkanReady, Qt::QueuedConnection);
}

void EquirectViewportRenderer::releaseResources()
{
    VkDevice device = m_window->device();

    vkDeviceWaitIdle(device);
    destroyInsetResources();
    destroyLensResources();
    destroyTextureResources();

    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
}

void EquirectViewportRenderer::statsFrameStart(bool newPicture)
{
    Stats &s = m_stats;
    const auto now = render::framestats::Clock::now();
    if (s.lastRender != render::framestats::Clock::time_point{})
        s.interval.add(render::framestats::MsBetween(s.lastRender, now));
    else
        s.windowStart = now;
    s.lastRender = now;
    if (newPicture) {
        if (s.rendersOfCurrent > 0)
            ++s.shownFor[std::min(s.rendersOfCurrent, 5) - 1];
        s.rendersOfCurrent = 0;
        ++s.newFrames;
    }
    ++s.rendersOfCurrent;
    const float pose[4] = {m_window->Yaw(), m_window->Pitch(), m_window->Roll(), m_window->FovY()};
    if (!std::equal(pose, pose + 4, s.lastPose)) {
        ++s.poseChanges;
        std::copy(pose, pose + 4, s.lastPose);
    }
}

void EquirectViewportRenderer::statsFrameEnd(render::framestats::Clock::time_point start)
{
    Stats &s = m_stats;
    const auto now = render::framestats::Clock::now();
    s.cpu.add(render::framestats::MsBetween(start, now));
    if (render::framestats::MsBetween(s.windowStart, now) < 1000.0)
        return;
    std::fprintf(stderr,
                 "[stats] viewport: renders %zu | interval mean %.2f p99 %.2f max %.2f ms | "
                 "cpu mean %.2f max %.2f ms | new frames %d, shown for 1/2/3/4/5+ renders: "
                 "%d/%d/%d/%d/%d | pose changes %d\n",
                 s.cpu.count(), s.interval.mean(), s.interval.pct(0.99), s.interval.max(),
                 s.cpu.mean(), s.cpu.max(), s.newFrames, s.shownFor[0], s.shownFor[1],
                 s.shownFor[2], s.shownFor[3], s.shownFor[4], s.poseChanges);
    s.interval.clear();
    s.cpu.clear();
    s.newFrames = s.poseChanges = 0;
    std::fill(std::begin(s.shownFor), std::end(s.shownFor), 0);
    s.windowStart = now;
}

void EquirectViewportRenderer::startNextFrame()
{
    const auto statsStart = render::framestats::Clock::now();
    VkCommandBuffer commandBuffer = m_window->currentCommandBuffer();
    ++m_frameCounter;
    // QVulkanWindow's default render pass has no external subpass dependency,
    // so its layout transitions at vkCmdBeginRenderPass aren't ordered after
    // (a) the swapchain acquire -- waited on at COLOR_ATTACHMENT_OUTPUT -- or
    // (b) the previous frame's store to the one depth-stencil image all
    // frames share (sync validation: WRITE_AFTER_READ vs
    // vkAcquireNextImageKHR, WRITE_AFTER_WRITE vs the last storeOp). One
    // barrier from those stages / writes to everything after covers both.
    VkMemoryBarrier attachments{};
    attachments.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    attachments.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    attachments.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &attachments, 0, nullptr, 0, nullptr);
    // Lens frames no frame in flight can still be reading go back to their ring.
    while (!m_retired.empty() && m_retired.front().releaseAt <= m_frameCounter)
        m_retired.pop_front();

    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::shared_ptr<const render::LensFrame> lens;
    const bool newPicture = m_window->TakePendingFrame(width, height, rgba, lens);
    if (render::framestats::Enabled())
        statsFrameStart(newPicture);
    if (newPicture) {
        if (lens) {
            retireLens(std::move(m_currentLens));
            m_currentLens = std::move(lens);
            m_lensMode = true;
        } else {
            // Upload recorded into this frame's own command buffer, before the
            // render pass begins -- no separate submit, no wait-idle.
            ensureTextureResources(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            const uint32_t slot = static_cast<uint32_t>(m_window->currentFrame());
            std::memcpy(m_stagingMapped[slot], rgba.data(), rgba.size());
            recordTextureCopy(commandBuffer, slot);
            m_lastWidth = width;
            m_lastHeight = height;
            m_lastRgba = std::move(rgba);
            retireLens(std::move(m_currentLens));
            m_currentLens.reset();
            m_lensMode = false;
        }
    }

    // Lens mode: this frame's descriptor set -> the current pair's (imported)
    // images and its stitch params.
    const uint32_t frameSlot = static_cast<uint32_t>(m_window->currentFrame());
    LensImport *imp = nullptr;
    if (m_lensMode && m_currentLens && m_lensCapable) {
        imp = importFor(*m_currentLens);
        if (!imp) {
            m_lensCapable = false;
            m_window->setLensFramesSupported(false); // the controller falls back
        }
    }
    if (imp) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            core::log::Info("[viewport] drawing the view straight from the fisheyes (%dx%d "
                            "lenses)\n",
                            m_currentLens->slot->lensW, m_currentLens->slot->lensH);
        }
        if (imp->acquiredFill != m_currentLens->fill) {
            // Take the freshly written images over from the exporting device.
            VkImageMemoryBarrier acq[render::LensSlot::kImages]{};
            for (int i = 0; i < render::LensSlot::kImages; ++i) {
                acq[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                acq[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                acq[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                acq[i].srcAccessMask = 0;
                acq[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                acq[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
                acq[i].dstQueueFamilyIndex = m_window->graphicsQueueFamilyIndex();
                acq[i].image = imp->img[i];
                acq[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                                             0, nullptr, render::LensSlot::kImages, acq);
            imp->acquiredFill = m_currentLens->fill;
        }
        imp->lastUsed = m_frameCounter;
        std::memcpy(m_lensUboMap[frameSlot], m_currentLens->params.data(),
                    render::LensFrame::kParamsBytes);
        VkDescriptorBufferInfo uboInfo{m_lensUbo[frameSlot], 0, render::LensFrame::kParamsBytes};
        VkDescriptorImageInfo imgInfo[4];
        VkWriteDescriptorSet w[5]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = m_lensSets[frameSlot];
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &uboInfo;
        for (int i = 0; i < 4; ++i) {
            imgInfo[i] = {m_lensSampler, imp->view[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[1 + i] = w[0];
            w[1 + i].dstBinding = static_cast<uint32_t>(1 + i);
            w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[1 + i].pBufferInfo = nullptr;
            w[1 + i].pImageInfo = &imgInfo[i];
        }
        vkUpdateDescriptorSets(m_window->device(), 5, w, 0, nullptr);
    }

    // Drop imports of slots not drawn for a while (a trimmed ring, a closed
    // clip). None of them is in flight any more.
    for (auto it = m_imports.begin(); it != m_imports.end();) {
        const bool current = m_currentLens && it->first == m_currentLens->slot->id;
        if (!current && it->second.lastUsed + 120 < m_frameCounter) {
            destroyImport(it->second);
            it = m_imports.erase(it);
        } else {
            ++it;
        }
    }

    // Insets: upload any new pictures (outside the render pass).
    std::vector<EquirectViewportWindow::InsetDraw> insets;
    if (m_insetPipeline != VK_NULL_HANDLE)
        insets = m_window->Insets();
    if (insets.size() > size_t(EquirectViewportWindow::kMaxInsets))
        insets.resize(EquirectViewportWindow::kMaxInsets);
    for (size_t i = 0; i < insets.size(); ++i) {
        const auto &d = insets[i];
        InsetSlot &slot = m_insetSlots[i];
        if (!d.rgba || d.width <= 0 || d.height <= 0 ||
            d.rgba->size() < size_t(d.width) * d.height * 4)
            continue;
        if (slot.key == d.frameKey && slot.w == uint32_t(d.width) && slot.h == uint32_t(d.height))
            continue;
        ensureInsetSlot(slot, uint32_t(d.width), uint32_t(d.height));
        recordInsetUpload(commandBuffer, slot, *d.rgba);
        slot.key = d.frameKey;
    }

    const QSize size = m_window->swapChainImageSize();

    // One clear value per attachment the default render pass clears: colour,
    // depth-stencil, and the multisample colour when MSAA is on
    // (VUID-VkRenderPassBeginInfo-clearValueCount-00902).
    VkClearValue clearValues[3]{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    clearValues[2].color = clearValues[0].color;

    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = m_window->defaultRenderPass();
    beginInfo.framebuffer = m_window->currentFramebuffer();
    beginInfo.renderArea.extent.width = static_cast<uint32_t>(size.width());
    beginInfo.renderArea.extent.height = static_cast<uint32_t>(size.height());
    beginInfo.clearValueCount = m_window->sampleCountFlagBits() > VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
    beginInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    if (imp) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lensPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_lensPipelineLayout, 0, 1, &m_lensSets[frameSlot], 0,
                                            nullptr);
    } else {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    }

    // Letterbox / pillarbox the picture to the project's output aspect: the
    // render pass clears the whole window to black, the draw covers only the
    // centred output-shaped rect.
    const float winAspect =
        size.height() > 0 ? static_cast<float>(size.width()) / size.height() : 1.0f;
    const float outAspect = m_window->OutputAspect() > 0.0f ? m_window->OutputAspect() : winAspect;
    int rectW = size.width();
    int rectH = size.height();
    if (outAspect > winAspect)
        rectH = std::max(1, static_cast<int>(std::lround(size.width() / outAspect)));
    else
        rectW = std::max(1, static_cast<int>(std::lround(size.height() * outAspect)));
    const int rectX = (size.width() - rectW) / 2;
    const int rectY = (size.height() - rectH) / 2;

    VkViewport viewport{};
    viewport.x = static_cast<float>(rectX);
    viewport.y = static_cast<float>(rectY);
    viewport.width = static_cast<float>(rectW);
    viewport.height = static_cast<float>(rectH);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset.x = rectX;
    scissor.offset.y = rectY;
    scissor.extent.width = static_cast<uint32_t>(rectW);
    scissor.extent.height = static_cast<uint32_t>(rectH);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    PushConstants pc{};
    pc.yaw = m_window->Yaw();
    pc.pitch = m_window->Pitch();
    pc.roll = m_window->Roll();
    pc.fovY = m_window->FovY();
    pc.aspect = outAspect;
    // Stabilization matrix, row-major in the window -> col-major (3 padded
    // vec3 columns) for std140.
    float stabRm[9];
    m_window->StabRowMajor(stabRm);
    for (int col = 0; col < 3; ++col) {
        pc.stab[col * 4 + 0] = stabRm[0 * 3 + col];
        pc.stab[col * 4 + 1] = stabRm[1 * 3 + col];
        pc.stab[col * 4 + 2] = stabRm[2 * 3 + col];
        pc.stab[col * 4 + 3] = 0.0f;
    }
    m_window->ColorGradeArr(pc.colorGrade);
    if (imp) {
        LensPushConstants lpc{};
        lpc.view = pc;
        lpc.gains[0] = m_currentLens->gain[0];
        lpc.gains[1] = m_currentLens->gain[1];
        vkCmdPushConstants(commandBuffer, m_lensPipelineLayout,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(lpc), &lpc);
    } else {
        vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &pc);
    }

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    // The privacy-blur region outlines, on the picture (under the insets):
    // each scissored to its own bounds, so a pass costs only its pixels.
    const auto &outlines = m_window->Outlines();
    if (m_regionPipeline && !outlines.empty()) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_regionPipeline);
        const float border = 2.0f * float(m_window->devicePixelRatio());
        for (const auto &o : outlines) {
            RegionPushConstants rpc{};
            const int n = projectOutline(o, stabRm, pc.yaw, pc.pitch, pc.roll, pc.fovY, outAspect,
                                         float(rectW), float(rectH), rpc.pts);
            if (n < 3)
                continue;
            float lo[2] = {rpc.pts[0], rpc.pts[1]}, hi[2] = {lo[0], lo[1]};
            for (int i = 1; i < n; ++i)
                for (int k = 0; k < 2; ++k) {
                    lo[k] = std::min(lo[k], rpc.pts[i * 2 + k]);
                    hi[k] = std::max(hi[k], rpc.pts[i * 2 + k]);
                }
            const float pad = border + 1.0f;
            const int x0 = std::max(0, int(std::floor(lo[0] - pad)));
            const int y0 = std::max(0, int(std::floor(lo[1] - pad)));
            const int x1 = std::min(rectW, int(std::ceil(std::min(hi[0] + pad, 1e6f))));
            const int y1 = std::min(rectH, int(std::ceil(std::min(hi[1] + pad, 1e6f))));
            if (x1 <= x0 || y1 <= y0)
                continue;
            VkRect2D rs{};
            rs.offset.x = rectX + x0;
            rs.offset.y = rectY + y0;
            rs.extent.width = static_cast<uint32_t>(x1 - x0);
            rs.extent.height = static_cast<uint32_t>(y1 - y0);
            vkCmdSetScissor(commandBuffer, 0, 1, &rs);
            rpc.origin[0] = float(rectX);
            rpc.origin[1] = float(rectY);
            rpc.border = border;
            rpc.count = float(n);
            std::copy(o.rgb, o.rgb + 3, rpc.color);
            rpc.color[3] = 1.0f;
            vkCmdPushConstants(commandBuffer, m_regionPipelineLayout,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rpc), &rpc);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        }
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    // The insets over the view, bottom-most first, placed in the output rect
    // exactly as the export places them (inset::place).
    bool insetBound = false;
    for (size_t i = 0; i < insets.size(); ++i) {
        const auto &d = insets[i];
        const InsetSlot &slot = m_insetSlots[i];
        if (!slot.img || slot.key != d.frameKey)
            continue;
        const inset::Rect r = inset::place(d.look, d.frame, d.aspect, rectW, rectH);
        if (r.empty() || d.look.opacity <= 0.0f)
            continue;
        if (!insetBound) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          m_insetPipeline);
            insetBound = true;
        }
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_insetPipelineLayout, 0, 1, &slot.set, 0, nullptr);
        InsetPushConstants ipc{};
        ipc.rect[0] = float(r.x);
        ipc.rect[1] = float(r.y);
        ipc.rect[2] = float(r.w);
        ipc.rect[3] = float(r.h);
        ipc.origin[0] = float(rectX);
        ipc.origin[1] = float(rectY);
        ipc.radius = float(r.radius);
        ipc.border = float(r.border);
        ipc.borderColor[0] = ((d.frame.borderRgb >> 16) & 0xff) / 255.0f;
        ipc.borderColor[1] = ((d.frame.borderRgb >> 8) & 0xff) / 255.0f;
        ipc.borderColor[2] = (d.frame.borderRgb & 0xff) / 255.0f;
        ipc.borderColor[3] = d.look.opacity;
        vkCmdPushConstants(commandBuffer, m_insetPipelineLayout,
                                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ipc), &ipc);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    // The Track subject box, over everything.
    float marker[4];
    EquirectViewportWindow::MarkerStyle markerStyle{};
    if (m_markerPipeline && m_window->Marker(marker, &markerStyle)) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_markerPipeline);
        MarkerPushConstants mpc{};
        mpc.rect[0] = marker[0] * rectW;
        mpc.rect[1] = marker[1] * rectH;
        mpc.rect[2] = marker[2] * rectW;
        mpc.rect[3] = marker[3] * rectH;
        mpc.origin[0] = float(rectX);
        mpc.origin[1] = float(rectY);
        mpc.border = 2.0f * float(m_window->devicePixelRatio());
        // Yellow while drawing, green while tracking, red while lost.
        static constexpr float kColors[3][3] = {{1.0f, 0.8f, 0.1f}, {0.2f, 0.9f, 0.3f},
                                                {1.0f, 0.25f, 0.2f}};
        const float *col = kColors[static_cast<int>(markerStyle)];
        mpc.fill = markerStyle == EquirectViewportWindow::MarkerStyle::Select ? 0.12f : 0.0f;
        mpc.color[0] = col[0];
        mpc.color[1] = col[1];
        mpc.color[2] = col[2];
        mpc.color[3] = 1.0f;
        vkCmdPushConstants(commandBuffer, m_markerPipelineLayout,
                                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mpc), &mpc);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);

    if (render::framestats::Enabled())
        statsFrameEnd(statsStart);
    m_window->frameReady();
    m_window->requestUpdate();
}

} // namespace

QVulkanWindowRenderer *EquirectViewportWindow::createRenderer()
{
    return new EquirectViewportRenderer(this);
}

void EquirectViewportWindow::SetFrame(int width, int height, std::vector<uint8_t> rgba,
                                      const float stab[9], std::vector<RegionOutline> outlines)
{
    QMutexLocker lock(&m_frameMutex);
    m_pendingWidth = width;
    m_pendingHeight = height;
    m_pendingRgba = std::move(rgba);
    m_pendingLens.reset();
    m_pendingOutlines = std::move(outlines);
    m_framePending = true;
    m_pendingHasStab = stab != nullptr;
    if (stab)
        std::copy(stab, stab + 9, m_pendingStab);
    m_pendingHasOverlay = false;
}

void EquirectViewportWindow::SetFrame(int width, int height, std::vector<uint8_t> rgba,
                                      const float stab[9], const FrameOverlay &overlay)
{
    SetFrame(width, height, std::move(rgba), stab);
    QMutexLocker lock(&m_frameMutex);
    m_pendingOverlay = overlay;
    m_pendingHasOverlay = true;
}

void EquirectViewportWindow::SetLensFrame(std::shared_ptr<const render::LensFrame> frame,
                                          const float stab[9], std::vector<RegionOutline> outlines)
{
    QMutexLocker lock(&m_frameMutex);
    m_pendingLens = std::move(frame); // a replaced pending frame goes straight back to its ring
    m_pendingRgba.clear();
    m_pendingOutlines = std::move(outlines);
    m_framePending = true;
    m_pendingHasStab = stab != nullptr;
    if (stab)
        std::copy(stab, stab + 9, m_pendingStab);
    m_pendingHasOverlay = false;
}

void EquirectViewportWindow::EndFrameOverlays()
{
    {
        QMutexLocker lock(&m_frameMutex);
        m_pendingHasOverlay = false;
    }
    ClearMarker();
}

bool EquirectViewportWindow::TakePendingFrame(int &width, int &height, std::vector<uint8_t> &rgba,
                                              std::shared_ptr<const render::LensFrame> &lens)
{
    QMutexLocker lock(&m_frameMutex);
    if (!m_framePending) return false;
    width = m_pendingWidth;
    height = m_pendingHeight;
    rgba = std::move(m_pendingRgba);
    lens = std::move(m_pendingLens);
    m_outlines = std::move(m_pendingOutlines);
    m_pendingOutlines.clear();
    m_framePending = false;
    if (m_pendingHasStab) // the renderer reads m_stab right after this, same thread
        std::copy(m_pendingStab, m_pendingStab + 9, m_stab);
    if (m_pendingHasOverlay) { // ... and the pose / marker
        const FrameOverlay &o = m_pendingOverlay;
        SetPose(o.pose[0], o.pose[1], o.pose[2], o.pose[3]);
        if (o.hasMarker)
            SetMarker(o.marker, o.markerStyle);
        else
            ClearMarker();
        m_pendingHasOverlay = false;
    }
    return true;
}

void EquirectViewportWindow::SetPose(float yaw, float pitch, float roll, float fovY)
{
    m_yaw = yaw;
    m_pitch = std::clamp(pitch, -kMaxPitch, kMaxPitch);
    m_roll = roll;
    m_fovY = std::clamp(fovY, kMinFovY, kMaxFovY);
}

void EquirectViewportWindow::SetStabilization(const float rowMajor3x3[9])
{
    for (int i = 0; i < 9; ++i)
        m_stab[i] = rowMajor3x3[i];
}

void EquirectViewportWindow::StabRowMajor(float out[9]) const
{
    for (int i = 0; i < 9; ++i)
        out[i] = m_stab[i];
}

void EquirectViewportWindow::SetColorGrade(const float grade[4])
{
    if (grade) {
        for (int i = 0; i < 4; ++i)
            m_colorGrade[i] = grade[i];
    } else {
        for (float &v : m_colorGrade)
            v = 0.0f;
    }
}

void EquirectViewportWindow::ColorGradeArr(float out[4]) const
{
    for (int i = 0; i < 4; ++i)
        out[i] = m_colorGrade[i];
}

void EquirectViewportWindow::SetInsets(std::vector<InsetDraw> insets)
{
    QMutexLocker lock(&m_insetMutex);
    m_insets = std::move(insets);
    if (m_clearOverride) {
        // Released: drop the override once the dropped position has come
        // back from the model (or the clip is gone, or it never does).
        bool back = ++m_clearOverrideTicks > 60;
        bool present = false;
        for (const InsetDraw &d : m_insets)
            if (d.clipId == m_overrideClipId) {
                present = true;
                back |= std::abs(d.look.centerX - m_overrideCx) < 1e-4f &&
                        std::abs(d.look.centerY - m_overrideCy) < 1e-4f;
            }
        if (back || !present) {
            m_clearOverride = false;
            m_overrideClipId = 0;
        }
    }
}

std::vector<EquirectViewportWindow::InsetDraw> EquirectViewportWindow::Insets() const
{
    QMutexLocker lock(&m_insetMutex);
    std::vector<InsetDraw> out = m_insets;
    if (m_overrideClipId)
        for (InsetDraw &d : out)
            if (d.clipId == m_overrideClipId) {
                d.look.centerX = m_overrideCx;
                d.look.centerY = m_overrideCy;
            }
    return out;
}

QRectF EquirectViewportWindow::outputRect() const
{
    const double w = width(), h = height();
    const double winAspect = h > 0 ? w / h : 1.0;
    const double outAspect = m_outputAspect > 0.0f ? m_outputAspect : winAspect;
    double rw = w, rh = h;
    if (outAspect > winAspect)
        rh = w / outAspect;
    else
        rw = h * outAspect;
    return QRectF((w - rw) / 2.0, (h - rh) / 2.0, rw, rh);
}

bool EquirectViewportWindow::sphereDirAt(QPointF pos, float out[3]) const
{
    const QRectF rect = outputRect();
    if (!rect.contains(pos) || rect.width() <= 0 || rect.height() <= 0)
        return false;
    SphereDirAtFraction((pos.x() - rect.x()) / rect.width(), (pos.y() - rect.y()) / rect.height(), out);
    return true;
}

void EquirectViewportWindow::SphereDirAtFraction(double fx, double fy, float out[3]) const
{
    const QRectF rect = outputRect();
    // equirect.frag's ray: view ray from the NDC, roll, pitch, yaw, then the
    // stabilization.
    const float ndcX = float(2.0 * fx - 1.0);
    const float ndcY = float(2.0 * fy - 1.0);
    const float t = std::tan(m_fovY * 0.5f);
    const float aspect = rect.height() > 0 ? float(rect.width() / rect.height()) : 1.0f;
    float v[3] = {ndcX * t * aspect, -ndcY * t, -1.0f};
    const float cr = std::cos(m_roll), sr = std::sin(m_roll);
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    const float x1 = v[0] * cr - v[1] * sr, y1 = v[0] * sr + v[1] * cr, z1 = v[2];
    const float y2 = y1 * cp - z1 * sp, z2 = y1 * sp + z1 * cp;
    const float p[3] = {x1 * cy + z2 * sy, y2, -x1 * sy + z2 * cy};
    float m[9];
    StabRowMajor(m);
    float n = 0.0f;
    for (int i = 0; i < 3; ++i) {
        out[i] = m[i * 3] * p[0] + m[i * 3 + 1] * p[1] + m[i * 3 + 2] * p[2];
        n += out[i] * out[i];
    }
    n = std::sqrt(n);
    for (int i = 0; i < 3; ++i)
        out[i] /= n;
}

int EquirectViewportWindow::insetAt(QPointF pos) const
{
    const QRectF out = outputRect();
    const std::vector<InsetDraw> insets = Insets();
    const int n = std::min<int>(static_cast<int>(insets.size()), kMaxInsets);
    for (int i = n - 1; i >= 0; --i) { // topmost first
        const InsetDraw &d = insets[size_t(i)];
        if (d.look.opacity <= 0.0f)
            continue;
        const inset::Rect r =
            inset::place(d.look, d.frame, d.aspect, int(std::lround(out.width())),
                         int(std::lround(out.height())));
        const double x = pos.x() - out.x(), y = pos.y() - out.y();
        if (!r.empty() && inset::shade(x, y, r).alpha > 0.5f)
            return d.clipId;
    }
    return 0;
}

void EquirectViewportWindow::SetBoxSelectMode(bool on)
{
    m_boxSelect = on;
    m_boxDragging = false;
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
}

void EquirectViewportWindow::SetMarker(const QRectF &fractions, MarkerStyle style)
{
    QMutexLocker lock(&m_insetMutex);
    m_marker = fractions;
    m_markerStyle = style;
    m_hasMarker = true;
}

void EquirectViewportWindow::ClearMarker()
{
    QMutexLocker lock(&m_insetMutex);
    m_hasMarker = false;
}

bool EquirectViewportWindow::Marker(float out[4], MarkerStyle *style) const
{
    QMutexLocker lock(&m_insetMutex);
    if (!m_hasMarker)
        return false;
    if (style)
        *style = m_markerStyle;
    out[0] = float(m_marker.x());
    out[1] = float(m_marker.y());
    out[2] = float(m_marker.width());
    out[3] = float(m_marker.height());
    return true;
}

void EquirectViewportWindow::mousePressEvent(QMouseEvent *event)
{
    if (m_boxSelect) {
        if (event->button() == Qt::LeftButton) {
            m_boxDragging = true;
            m_boxStart = event->position();
            ClearMarker();
        }
        return;
    }
    // A press on an inset picks it up to move it, instead of looking
    // around.
    if (event->button() == Qt::LeftButton) {
        if (const int clipId = insetAt(event->position())) {
            for (const InsetDraw &d : Insets())
                if (d.clipId == clipId) {
                    m_insetDragCx0 = d.look.centerX;
                    m_insetDragCy0 = d.look.centerY;
                }
            m_insetDragClipId = clipId;
            m_insetDragStart = event->position();
            m_insetDragMoved = false;
            emit insetPressed(clipId);
            return;
        }
    }
    m_dragging = true;
    m_dragButton = event->button();
    m_lastMousePos = event->pos();
    m_pressYaw = m_yaw;
    m_pressPitch = m_pitch;
    emit draggingChanged(true);
}

void EquirectViewportWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_boxSelect) {
        if (m_boxDragging) {
            const QRectF out = outputRect();
            auto frac = [&out](QPointF p) {
                return QPointF(std::clamp((p.x() - out.x()) / std::max(1.0, out.width()), 0.0, 1.0),
                               std::clamp((p.y() - out.y()) / std::max(1.0, out.height()), 0.0, 1.0));
            };
            SetMarker(QRectF(frac(m_boxStart), frac(event->position())).normalized());
        }
        return;
    }
    if (m_insetDragClipId) {
        const QRectF out = outputRect();
        const QPointF d = event->position() - m_insetDragStart;
        if (!m_insetDragMoved && d.manhattanLength() < 3.0)
            return;
        m_insetDragMoved = true;
        inset::Look l;
        l.centerX = m_insetDragCx0 + float(d.x() / std::max(1.0, out.width()));
        l.centerY = m_insetDragCy0 + float(d.y() / std::max(1.0, out.height()));
        l = l.clamped();
        QMutexLocker lock(&m_insetMutex);
        m_overrideClipId = m_insetDragClipId;
        m_overrideCx = l.centerX;
        m_overrideCy = l.centerY;
        m_clearOverride = false;
        return;
    }
    if (!event->buttons()) { // hover: show that an inset can be moved
        setCursor(insetAt(event->position()) ? Qt::SizeAllCursor : Qt::ArrowCursor);
        return;
    }
    if (!m_dragging) return;
    const QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    m_yaw -= delta.x() * kMouseSensitivity;
    m_pitch = std::clamp(m_pitch - delta.y() * kMouseSensitivity, -kMaxPitch, kMaxPitch);
}

void EquirectViewportWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_boxSelect) {
        if (!m_boxDragging || event->button() != Qt::LeftButton)
            return;
        m_boxDragging = false;
        float m[4];
        const QRectF out = outputRect();
        // At least a few pixels each way, or it was a click: cancel.
        if (Marker(m) && m[2] * out.width() >= 6.0 && m[3] * out.height() >= 6.0) {
            SetBoxSelectMode(false);
            emit boxSelected(QRectF(m[0], m[1], m[2], m[3]));
        } else {
            ClearMarker();
            SetBoxSelectMode(false);
            emit boxSelectCanceled();
        }
        return;
    }
    if (m_insetDragClipId) {
        const int clipId = m_insetDragClipId;
        m_insetDragClipId = 0;
        float cx = 0.0f, cy = 0.0f;
        {
            QMutexLocker lock(&m_insetMutex);
            cx = m_overrideCx;
            cy = m_overrideCy;
            if (m_insetDragMoved) {
                m_clearOverride = true; // until the model's position comes back
                m_clearOverrideTicks = 0;
            }
            else
                m_overrideClipId = 0;
        }
        if (m_insetDragMoved)
            emit insetMoved(clipId, cx, cy);
        return;
    }
    if (!m_dragging) return;
    m_dragging = false;
    emit draggingChanged(false);
    if (m_yaw != m_pressYaw || m_pitch != m_pressPitch) {
        emit lookCommitted();
        return;
    }
    // A right click that didn't look around: the picture's context menu.
    float dir[3];
    if (m_dragButton == Qt::RightButton && event->button() == Qt::RightButton &&
        sphereDirAt(event->position(), dir))
        emit pictureContextMenu(event->globalPosition().toPoint(), {dir[0], dir[1], dir[2]});
}

void EquirectViewportWindow::keyPressEvent(QKeyEvent *event)
{
    if (m_boxSelect && event->key() == Qt::Key_Escape) {
        ClearMarker();
        SetBoxSelectMode(false);
        emit boxSelectCanceled();
        return;
    }
    QVulkanWindow::keyPressEvent(event);
}

void EquirectViewportWindow::wheelEvent(QWheelEvent *event)
{
    if (m_boxSelect)
        return;
    const float notches = event->angleDelta().y() / 120.0f;
    const float before = m_fovY;
    m_fovY = std::clamp(m_fovY - notches * kWheelFovStep, kMinFovY, kMaxFovY);
    if (m_fovY != before)
        emit lookCommitted();
}
