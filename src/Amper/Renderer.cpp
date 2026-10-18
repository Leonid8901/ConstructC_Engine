#include "Amper/Renderer.h"
#include "Core.h"
#include "Platform/Window.h"
#include "UniX/GpuLanes.h"
#include "UniX/Scheduler.h"
#include "Vulkan/VkCheck.h"
#include "Vulkan/VulkanContext.h"
#include "World/WorldImpl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>

namespace ce::core {

namespace {

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

VkShaderModule LoadShader(VkDevice device, const std::string& name) {
    // Сначала рядом с exe (для раздачи игры), потом каталог сборки
    std::string path = "shaders/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        path = std::string(SHADER_DIR) + name;
        file.open(path, std::ios::binary | std::ios::ate);
    }
    if (!file)
        throw std::runtime_error("Cannot open shader: " + name + " (shaders/ рядом с exe или " + SHADER_DIR + ")");
    size_t size = static_cast<size_t>(file.tellg());
    if (size == 0 || size % 4 != 0)
        throw std::runtime_error("Invalid SPIR-V: " + path);
    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code.data();
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess, uint32_t layers = 1) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void MemBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

Renderer::Renderer(const VulkanContext& ctx, Gpu& gpu, GpuLanes& lanes, Window& window, const EngineConfig& config)
    : m_ctx(ctx), m_gpu(gpu), m_lanes(lanes), m_window(window), m_device(ctx.Device()),
      m_rtSupported(ctx.RayTracingSupported()) {
    m_rtEnabled.store(config.rayTracing && m_rtSupported);
    m_swapchain = std::make_unique<Swapchain>(ctx, window, config.vsync);
    m_meshes = std::make_unique<MeshRegistry>(gpu, lanes);
    CreateDepth();
    CreateShadowResources();
    CreateDescriptors();
    CreateFrames();
    CreateRenderFinished();
    CreatePipelines();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(m_device);
    for (Frame& f : m_frames) {
        vkDestroyCommandPool(m_device, f.pool, nullptr);
        vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
        vkDestroyFence(m_device, f.fence, nullptr);
        m_gpu.Destroy(f.ubo);
        m_gpu.Destroy(f.instances);
        m_gpu.Destroy(f.lights);
        if (f.tlas)
            m_gpu.Rt().destroyAs(m_device, f.tlas, nullptr);
        m_gpu.Destroy(f.tlasInstances);
        m_gpu.Destroy(f.tlasBuffer);
        m_gpu.Destroy(f.scratch);
    }
    DestroyRenderFinished();
    vkDestroyPipeline(m_device, m_skyPipeline, nullptr);
    vkDestroyPipeline(m_device, m_meshPipeline, nullptr);
    vkDestroyPipeline(m_device, m_shadowPipeline, nullptr);
    if (m_meshRtPipeline)
        vkDestroyPipeline(m_device, m_meshRtPipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    for (VkImageView v : m_shadowLayerViews)
        vkDestroyImageView(m_device, v, nullptr);
    vkDestroySampler(m_device, m_shadowSampler, nullptr);
    m_gpu.Destroy(m_shadowMap);
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_gpu.Destroy(m_depth);
    m_meshes.reset();
    m_swapchain.reset();
}

// ---------------- создание ресурсов ----------------

void Renderer::CreateDepth() {
    m_gpu.Destroy(m_depth);
    m_depth = m_gpu.CreateImage2D(m_swapchain->Extent(), kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Renderer::CreateShadowResources() {
    m_shadowMap = m_gpu.CreateImage2D({kShadowSize, kShadowSize}, kDepthFormat,
                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      VK_IMAGE_ASPECT_DEPTH_BIT, kCascades);
    for (uint32_t c = 0; c < kCascades; ++c) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_shadowMap.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kDepthFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};
        VK_CHECK(vkCreateImageView(m_device, &vi, nullptr, &m_shadowLayerViews[c]));
    }

    // Аппаратное сравнение глубины + билинейная фильтрация = бесплатный 2x2 PCF на каждый тап
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;  // за краем карты — освещено
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(m_device, &si, nullptr, &m_shadowSampler));

    // Дескриптор статически использует карту, поэтому она всегда должна быть в читаемом layout
    m_lanes.Immediate(GpuLane::Graphics, [&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, m_shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCascades);
    });
}

void Renderer::CreateDescriptors() {
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    bindings.push_back({4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    if (m_rtSupported)
        bindings.push_back({3, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout));

    std::vector<VkDescriptorPoolSize> sizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight},
    };
    if (m_rtSupported)
        sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight});
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kFramesInFlight;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool));
}

void Renderer::CreateFrames() {
    for (Frame& f : m_frames) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = m_lanes.Family(GpuLane::Graphics);
        VK_CHECK(vkCreateCommandPool(m_device, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(m_device, &ai, &f.cmd));

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(m_device, &si, nullptr, &f.imageAvailable));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(m_device, &fi, nullptr, &f.fence));

        f.ubo = m_gpu.CreateBuffer(sizeof(FrameUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemUsage::Upload);
        f.lights = m_gpu.CreateBuffer(sizeof(GpuLight) * kMaxLights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemUsage::Upload);

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = m_descriptorPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &m_setLayout;
        VK_CHECK(vkAllocateDescriptorSets(m_device, &dai, &f.set));

        EnsureInstanceCapacity(f, 1024);
        WriteDescriptor(f, 0);
        WriteDescriptor(f, 2);
        WriteDescriptor(f, 4);
        if (m_rtSupported)
            EnsureTlasCapacity(f, 64);
    }
}

void Renderer::WriteDescriptor(Frame& f, uint32_t binding) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = f.set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    VkDescriptorBufferInfo bi{};
    VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    VkDescriptorImageInfo ii{m_shadowSampler, m_shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    switch (binding) {
    case 0: bi = {f.ubo.buffer, 0, VK_WHOLE_SIZE}; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi; break;
    case 1: bi = {f.instances.buffer, 0, VK_WHOLE_SIZE}; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi; break;
    case 2: bi = {f.lights.buffer, 0, VK_WHOLE_SIZE}; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi; break;
    case 3:
        as.accelerationStructureCount = 1;
        as.pAccelerationStructures = &f.tlas;
        w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w.pNext = &as;
        break;
    case 4:
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        break;
    }
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
}

void Renderer::EnsureInstanceCapacity(Frame& f, uint32_t count) {
    if (count <= f.instanceCapacity)
        return;
    uint32_t cap = std::max(f.instanceCapacity, 1024u);
    while (cap < count)
        cap *= 2;
    m_gpu.Destroy(f.instances);
    f.instances = m_gpu.CreateBuffer(sizeof(GpuInstance) * cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemUsage::Upload);
    f.instanceCapacity = cap;
    WriteDescriptor(f, 1);
}

void Renderer::EnsureTlasCapacity(Frame& f, uint32_t count) {
    if (count <= f.tlasCapacity && f.tlas)
        return;
    uint32_t cap = std::max(f.tlasCapacity, 64u);
    while (cap < count)
        cap *= 2;
    const RtFunctions& rt = m_gpu.Rt();
    if (f.tlas)
        rt.destroyAs(m_device, f.tlas, nullptr);
    m_gpu.Destroy(f.tlasInstances);
    m_gpu.Destroy(f.tlasBuffer);
    m_gpu.Destroy(f.scratch);

    f.tlasInstances = m_gpu.CreateBuffer(sizeof(VkAccelerationStructureInstanceKHR) * cap,
                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         MemUsage::Upload, false, 16);

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.data.deviceAddress = f.tlasInstances.address;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt.buildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &cap, &sizes);

    f.tlasBuffer = m_gpu.CreateBuffer(sizes.accelerationStructureSize,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      MemUsage::GpuOnly);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = f.tlasBuffer.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VK_CHECK(rt.createAs(m_device, &ci, nullptr, &f.tlas));
    f.scratch = m_gpu.CreateBuffer(sizes.buildScratchSize,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   MemUsage::GpuOnly, false, m_gpu.ScratchAlignment());
    f.tlasCapacity = cap;
    WriteDescriptor(f, 3);
}

void Renderer::CreatePipelines() {
    m_colorFormat = m_swapchain->Format();

    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &m_setLayout;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t)};  // индекс каскада
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(m_device, &li, nullptr, &m_pipelineLayout));

    auto build = [&](const char* vs, const char* fs, bool mesh, bool shadow = false) {
        VkShaderModule vert = LoadShader(m_device, vs);
        VkShaderModule frag = fs ? LoadShader(m_device, fs) : VK_NULL_HANDLE;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};

        VkVertexInputBindingDescription vb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[2] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (mesh) {
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &vb;
            vi.vertexAttributeDescriptionCount = shadow ? 1 : 2;
            vi.pVertexAttributeDescriptions = attrs;
        }
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = mesh ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        if (shadow) {
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.depthBiasEnable = VK_TRUE;  // против «акне»; основное смещение — normal offset в шейдере
            rs.depthBiasConstantFactor = 1.0f;
            rs.depthBiasSlopeFactor = 1.75f;
        }
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = mesh ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = mesh ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = shadow ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER;  // кадр — reverse-Z
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = shadow ? 0 : 1;
        cb.pAttachments = &ba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = shadow ? 0 : 1;
        ri.pColorAttachmentFormats = shadow ? nullptr : &m_colorFormat;
        ri.depthAttachmentFormat = kDepthFormat;

        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext = &ri;
        pi.stageCount = frag ? 2 : 1;
        pi.pStages = stages;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = m_pipelineLayout;
        VkPipeline p;
        VkResult r = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
        vkDestroyShaderModule(m_device, vert, nullptr);
        if (frag)
            vkDestroyShaderModule(m_device, frag, nullptr);
        VK_CHECK(r);
        return p;
    };

    m_skyPipeline = build("sky.vert.spv", "sky.frag.spv", false);
    m_meshPipeline = build("mesh.vert.spv", "mesh.frag.spv", true);
    m_shadowPipeline = build("shadow.vert.spv", nullptr, true, true);
    if (m_rtSupported)
        m_meshRtPipeline = build("mesh.vert.spv", "mesh_rt.frag.spv", true);
}

void Renderer::CreateRenderFinished() {
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    m_renderFinished.resize(m_swapchain->ImageCount());
    for (auto& s : m_renderFinished)
        VK_CHECK(vkCreateSemaphore(m_device, &si, nullptr, &s));
}

void Renderer::DestroyRenderFinished() {
    for (VkSemaphore s : m_renderFinished)
        vkDestroySemaphore(m_device, s, nullptr);
    m_renderFinished.clear();
}

void Renderer::HandleResize() {
    m_swapchain->Recreate();  // внутри vkDeviceWaitIdle
    DestroyRenderFinished();
    CreateRenderFinished();
    CreateDepth();
    if (m_swapchain->Format() != m_colorFormat) {
        vkDestroyPipeline(m_device, m_skyPipeline, nullptr);
        vkDestroyPipeline(m_device, m_meshPipeline, nullptr);
        vkDestroyPipeline(m_device, m_shadowPipeline, nullptr);
        if (m_meshRtPipeline)
            vkDestroyPipeline(m_device, m_meshRtPipeline, nullptr);
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_meshRtPipeline = VK_NULL_HANDLE;
        CreatePipelines();
    }
}

// ---------------- освещение ----------------

void Renderer::SetSun(Vec3 dir, Vec3 color, float intensity) {
    std::lock_guard lk(m_settingsMutex);
    m_sunDir = Normalize(dir);
    m_sunColor = color;
    m_sunIntensity = intensity;
}
void Renderer::SetAmbient(Vec3 color, float intensity) {
    std::lock_guard lk(m_settingsMutex);
    m_ambient = color;
    m_ambientIntensity = intensity;
}
void Renderer::SetSky(Vec3 zenith, Vec3 horizon) {
    std::lock_guard lk(m_settingsMutex);
    m_skyZenith = zenith;
    m_skyHorizon = horizon;
}
void Renderer::SetShadowDistance(float d) {
    std::lock_guard lk(m_settingsMutex);
    m_shadowDistance = std::clamp(d, 5.0f, 1000.0f);
}

// Каскады: «практическое» разбиение (смесь логарифмического и линейного), на каждый каскад —
// ограничивающая сфера среза фрустума (размер не зависит от поворота камеры) и привязка к сетке
// текселей (тени не «мерцают» при движении).
void Renderer::ComputeCascades(FrameUbo& ubo, Vec3 camPos, Vec3 fwd, Vec3 right, Vec3 up, float tanHalf, float aspect,
                               Vec3 sunDir, float distance) {
    constexpr float kNear = 0.05f, kLambda = 0.75f;
    float splits[kCascades + 1];
    splits[0] = kNear;
    for (uint32_t i = 1; i <= kCascades; ++i) {
        float p = static_cast<float>(i) / kCascades;
        splits[i] = kLambda * kNear * std::pow(distance / kNear, p) + (1 - kLambda) * (kNear + (distance - kNear) * p);
    }

    Vec3 L = Normalize(sunDir);
    Vec3 upL = std::fabs(L.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    Mat4 lightRot = Mat4::LookAt({0, 0, 0}, L, upL);

    for (uint32_t c = 0; c < kCascades; ++c) {
        Vec3 corners[8];
        int k = 0;
        for (float d : {splits[c], splits[c + 1]}) {
            Vec3 cc = camPos + fwd * d;
            float hh = d * tanHalf, hw = hh * aspect;
            for (float sx : {-1.0f, 1.0f})
                for (float sy : {-1.0f, 1.0f})
                    corners[k++] = cc + right * (sx * hw) + up * (sy * hh);
        }
        Vec3 center{};
        for (Vec3 p : corners)
            center += p;
        center = center / 8.0f;
        float r = 0;
        for (Vec3 p : corners)
            r = std::max(r, Length(p - center));
        r = std::ceil(r * 16.0f) / 16.0f;

        float texel = 2.0f * r / kShadowSize;
        Vec3 cl = lightRot.TransformPoint(center);
        cl.x = std::floor(cl.x / texel) * texel;
        cl.y = std::floor(cl.y / texel) * texel;
        // Ближняя плоскость может отсечь далёкие тенеотбрасыватели — их спасает «пэнкейкинг» в shadow.vert
        Mat4 ortho = Mat4::Ortho(cl.x - r, cl.x + r, cl.y - r, cl.y + r, -cl.z - r, -cl.z + r);
        ubo.cascadeViewProj[c] = ortho * lightRot;
        ubo.cascadeSplits[c] = splits[c + 1];
        ubo.cascadeTexel[c] = texel;
    }
}

void Renderer::RecordShadowPass(Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    ImageBarrier(cmd, m_shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                 kCascades);

    VkViewport viewport{0, 0, static_cast<float>(kShadowSize), static_cast<float>(kShadowSize), 0, 1};
    VkRect2D scissor{{0, 0}, {kShadowSize, kShadowSize}};
    for (uint32_t c = 0; c < kCascades; ++c) {
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_shadowLayerViews[c];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &c);
        for (const Draw& d : f.draws) {
            const MeshGpu* mesh = m_meshes->Get(d.mesh);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices.buffer, &offset);
            vkCmdBindIndexBuffer(cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh->indexCount, d.count, 0, 0, d.first);
        }
        vkCmdEndRendering(cmd);
    }

    ImageBarrier(cmd, m_shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCascades);
}

void Renderer::SetExposure(float e) {
    std::lock_guard lk(m_settingsMutex);
    m_exposure = e;
}

Light Renderer::AddLight(const GpuLight& l) {
    std::lock_guard lk(m_settingsMutex);
    uint32_t idx;
    if (!m_freeLights.empty()) {
        idx = m_freeLights.back();
        m_freeLights.pop_back();
    } else {
        idx = static_cast<uint32_t>(m_lights.size());
        m_lights.emplace_back();
    }
    LightSlot& s = m_lights[idx];
    s.data = l;
    s.alive = true;
    return Access::MakeLight(idx, s.gen);
}

bool Renderer::LightValid(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_settingsMutex);
    return index < m_lights.size() && m_lights[index].alive && m_lights[index].gen == gen;
}

void Renderer::RemoveLight(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_settingsMutex);
    if (index < m_lights.size() && m_lights[index].alive && m_lights[index].gen == gen) {
        m_lights[index].alive = false;
        ++m_lights[index].gen;
        m_freeLights.push_back(index);
    }
}

uint32_t Renderer::LightCount() {
    std::lock_guard lk(m_settingsMutex);
    return static_cast<uint32_t>(m_lights.size() - m_freeLights.size());
}

// ---------------- Prepare (воркеры) ----------------

void Renderer::Prepare(Task& task) {
    Frame& f = m_frames[m_frameIndex];
    int64_t w0 = NowNs();
    VK_CHECK(vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    Core().scheduler->NoteWait(NowNs() - w0);

    WorldImpl& world = *Core().world;
    const uint32_t cap = world.Capacity();
    const uint32_t meshCount = m_meshes->Count();

    // 1. Ранг каждой сущности внутри своего меша (последовательно, дёшево)
    m_slot.resize(cap);
    m_meshCounts.assign(meshCount, 0);
    for (uint32_t i = 0; i < cap; ++i) {
        const EntityData& d = world.At(i);
        if (!d.alive || !d.visible || d.mesh >= meshCount || !m_meshes->Get(d.mesh)) {
            m_slot[i] = UINT32_MAX;
            continue;
        }
        m_slot[i] = m_meshCounts[d.mesh]++;
    }
    m_meshOffsets.resize(meshCount);
    uint32_t total = 0;
    f.draws.clear();
    for (uint32_t m = 0; m < meshCount; ++m) {
        m_meshOffsets[m] = total;
        if (m_meshCounts[m] > 0)
            f.draws.push_back({m, total, m_meshCounts[m]});
        total += m_meshCounts[m];
    }

    f.rt = m_rtEnabled.load() && m_rtSupported;
    f.shadows = !f.rt && m_shadowsEnabled.load();  // при RT тени даёт ray query, карты не нужны
    EnsureInstanceCapacity(f, std::max(total, 1u));
    if (f.rt)
        EnsureTlasCapacity(f, std::max(total, 1u));
    f.tlasCount = f.rt ? total : 0;

    // 2. Матрицы и TLAS-инстансы (параллельно)
    auto* instances = static_cast<GpuInstance*>(f.instances.mapped);
    auto* tlas = f.rt ? static_cast<VkAccelerationStructureInstanceKHR*>(f.tlasInstances.mapped) : nullptr;
    task.ParallelFor(cap, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            if (m_slot[i] == UINT32_MAX)
                continue;
            const EntityData& d = world.At(i);
            const uint32_t idx = m_meshOffsets[d.mesh] + m_slot[i];
            GpuInstance& g = instances[idx];
            Mat4 model = Mat4::TRS(d.pos, d.rot, d.scale);
            std::memcpy(g.model, model.m, sizeof(g.model));
            g.invScale2 = {1.0f / (d.scale.x * d.scale.x), 1.0f / (d.scale.y * d.scale.y), 1.0f / (d.scale.z * d.scale.z), 0};
            g.colorRough = {d.material.color, d.material.roughness};
            g.emissiveMetal = {d.material.emissive, d.material.metallic};
            if (tlas) {
                VkAccelerationStructureInstanceKHR& ti = tlas[idx];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        ti.transform.matrix[r][c] = model.m[c * 4 + r];
                ti.instanceCustomIndex = 0;
                ti.mask = 0xFF;
                ti.instanceShaderBindingTableRecordOffset = 0;
                ti.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
                ti.accelerationStructureReference = m_meshes->Get(d.mesh)->blasAddress;
            }
        }
    });

    // 3. Камера, свет
    Vec3 camPos;
    float yaw, pitch, fov;
    {
        CameraState& cam = *Core().camera;
        std::lock_guard lk(cam.mutex);
        camPos = cam.pos;
        yaw = cam.yaw;
        pitch = cam.pitch;
        fov = Radians(cam.fovDeg);
    }
    Vec3 fwd{std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};
    Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
    Vec3 up = Cross(right, fwd);
    VkExtent2D ext = m_swapchain->Extent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(std::max(ext.height, 1u));

    FrameUbo ubo{};
    ubo.viewProj = Mat4::PerspectiveReverseZ(fov, aspect, 0.05f) * Mat4::LookAt(camPos, camPos + fwd, {0, 1, 0});
    ubo.camPos = {camPos, static_cast<float>(Core().time)};
    ubo.camForward = {fwd, std::tan(fov * 0.5f)};
    ubo.camRight = {right, aspect};
    ubo.camUp = {up, 0};
    {
        std::lock_guard lk(m_settingsMutex);
        ubo.sunDir = {m_sunDir, m_sunIntensity};
        ubo.sunColor = {m_sunColor, 0};
        ubo.ambient = {m_ambient * m_ambientIntensity, m_exposure};
        ubo.skyZenith = {m_skyZenith, 0};
        ubo.skyHorizon = {m_skyHorizon, 0};
        if (f.shadows)
            ComputeCascades(ubo, camPos, fwd, right, up, std::tan(fov * 0.5f), aspect, m_sunDir, m_shadowDistance);
        ubo.shadowParams = {f.shadows ? 1.0f : 0.0f, 0, 0, 0};
        auto* lights = static_cast<GpuLight*>(f.lights.mapped);
        uint32_t n = 0;
        for (const LightSlot& s : m_lights)
            if (s.alive && n < kMaxLights)
                lights[n++] = s.data;
        ubo.lightCount = n;
    }
    ubo.rtEnabled = f.rt ? 1 : 0;
    std::memcpy(f.ubo.mapped, &ubo, sizeof(ubo));

    m_drawCalls.store(static_cast<uint32_t>(f.draws.size()));
    m_instances.store(total);
}

// ---------------- Render (главный поток) ----------------

void Renderer::Record(Frame& f, uint32_t imageIndex) {
    VkCommandBuffer cmd = f.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    // Ресурсы, загруженные через transfer/compute лейны, делаем видимыми графической очереди
    if (m_meshes->ConsumeNewResources())
        MemBarrier(cmd, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                          (m_rtSupported ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : 0),
                      VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                          (m_rtSupported ? VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR : 0));

    if (f.rt) {
        VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geom.geometry.instances.data.deviceAddress = f.tlasInstances.address;
        VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geom;
        build.dstAccelerationStructure = f.tlas;
        build.scratchData.deviceAddress = f.scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = f.tlasCount;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        m_gpu.Rt().cmdBuild(cmd, 1, &build, &ranges);
        MemBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &f.set, 0, nullptr);
    if (f.shadows)
        RecordShadowPass(f);

    VkImage image = m_swapchain->Image(imageIndex);
    VkExtent2D extent = m_swapchain->Extent();
    ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    ImageBarrier(cmd, m_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_swapchain->View(imageIndex);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // небо закрашивает весь экран
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {0.0f, 0};  // reverse-Z

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.rt ? m_meshRtPipeline : m_meshPipeline);
    for (const Draw& d : f.draws) {
        const MeshGpu* mesh = m_meshes->Get(d.mesh);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, mesh->indexCount, d.count, 0, 0, d.first);  // один вызов на меш
    }

    vkCmdEndRendering(cmd);
    ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::Render() {
    Frame& f = m_frames[m_frameIndex];

    uint32_t imageIndex = 0;
    int64_t w0 = NowNs();
    VkResult acq = vkAcquireNextImageKHR(m_device, m_swapchain->Handle(), UINT64_MAX, f.imageAvailable,
                                         VK_NULL_HANDLE, &imageIndex);
    Core().scheduler->NoteWait(NowNs() - w0);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        HandleResize();
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
        VK_CHECK(acq);

    VK_CHECK(vkResetFences(m_device, 1, &f.fence));
    VK_CHECK(vkResetCommandPool(m_device, f.pool, 0));
    Record(f, imageIndex);

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = m_renderFinished[imageIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(m_lanes.Submit(GpuLane::Graphics, si, f.fence));

    VkSwapchainKHR sc = m_swapchain->Handle();
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_renderFinished[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &imageIndex;
    w0 = NowNs();
    VkResult pres = m_lanes.Present(pi);
    Core().scheduler->NoteWait(NowNs() - w0);

    bool resized = m_window.ConsumeResized();
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR || resized)
        HandleResize();
    else if (pres != VK_SUCCESS)
        VK_CHECK(pres);

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

} // namespace ce::core
