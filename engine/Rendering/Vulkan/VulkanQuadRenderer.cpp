// VulkanQuadRenderer.cpp - Vulkan instanced quad renderer implementation
#include "VulkanQuadRenderer.h"
#include "VulkanContext.h"
#include "World/VoxelChunk.h"
#include "World/BlockType.h"
#include "Math/Vec3.h"
#include <iostream>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>

#include "../../libs/stb/stb_image.h"

// Quad vertex data (unit quad in XY plane, centered at origin)
struct QuadVertex {
    glm::vec3 position;
    glm::vec2 texCoord;
};

static const std::array<QuadVertex, 4> UNIT_QUAD_VERTICES = {{
    {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}},  // Bottom-left
    {{ 0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}},  // Bottom-right
    {{ 0.5f,  0.5f, 0.0f}, {1.0f, 1.0f}},  // Top-right
    {{-0.5f,  0.5f, 0.0f}, {0.0f, 1.0f}}   // Top-left
}};

static const std::array<uint32_t, 6> UNIT_QUAD_INDICES = {
    0, 1, 2,  // First triangle
    2, 3, 0   // Second triangle
};

VulkanQuadRenderer::VulkanQuadRenderer() {}

VulkanQuadRenderer::~VulkanQuadRenderer() {
    shutdown();
}

bool VulkanQuadRenderer::initialize(VulkanContext* ctx) {
    m_context = ctx;
    m_allocator = ctx->allocator;

    if (!m_allocator) {
        std::cerr << "[VulkanQuadRenderer] FATAL: VMA allocator is null!\n";
        return false;
    }

    detectGPUArchitecture();
    createUnitQuad();
    uploadUnitQuadData();
    createShaders();
    createDescriptorSetLayout();
    
    // Load block textures BEFORE creating pipelines and descriptors
    if (!loadBlockTextureArray()) {
        std::cerr << "[VulkanQuadRenderer] WARNING: Failed to load block textures, using placeholder\n";
    }
    
    createPipeline();
    createSwapchainPipeline();  // Phase 2 testing
    // createDepthOnlyPipeline() deferred until shadow map render pass available

    // Create persistent instance buffer (64MB) - SSBO for vertex pulling
    constexpr size_t INSTANCE_BUFFER_SIZE = 64 * 1024 * 1024;
    m_instanceBufferCapacity = INSTANCE_BUFFER_SIZE / sizeof(QuadFace);

    m_instanceBuffer = std::make_unique<VulkanBuffer>();
    m_instanceBuffer->create(
        m_allocator,
        INSTANCE_BUFFER_SIZE,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0
    );

    // Create island transform SSBO (max 1024 islands * 64 bytes = 64KB)
    // Use device-local with transfer (update via vkCmdUpdateBuffer)
    constexpr size_t MAX_ISLANDS = 1024;
    constexpr size_t TRANSFORM_BUFFER_SIZE = MAX_ISLANDS * sizeof(glm::mat4);
    m_islandTransformBuffer = std::make_unique<VulkanBuffer>();
    if (!m_islandTransformBuffer->create(
        m_allocator,
        TRANSFORM_BUFFER_SIZE,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0
    )) {
        std::cerr << "[VulkanQuadRenderer] FATAL: Failed to create island transform buffer!\n";
        return false;
    }

    std::cout << "[VulkanQuadRenderer] Vertex pulling enabled - instance buffer is SSBO\n";
    std::cout << "[VulkanQuadRenderer] Using vkCmdUpdateBuffer for dynamic updates (GPU-only memory)\n";

    // Create descriptors AFTER buffers are created
    std::cout << "[VulkanQuadRenderer] Creating descriptor pool...\n";
    createDescriptorPool();
    std::cout << "[VulkanQuadRenderer] Updating descriptor sets...\n";
    updateDescriptorSets();
    std::cout << "[VulkanQuadRenderer] Descriptors configured\n";

    std::cout << "[VulkanQuadRenderer] Initialized successfully\n";
    return true;
}

void VulkanQuadRenderer::shutdown() {
    if (!m_context || !m_context->device) {
        return;  // Already cleaned up
    }
    
    // Wait for GPU to finish before destroying resources (necessary during shutdown)
    vkDeviceWaitIdle(m_context->device);

    if (m_gbufferPipeline) {
        vkDestroyPipeline(m_context->device, m_gbufferPipeline, nullptr);
        m_gbufferPipeline = VK_NULL_HANDLE;
    }
    if (m_swapchainPipeline) {
        vkDestroyPipeline(m_context->device, m_swapchainPipeline, nullptr);
        m_swapchainPipeline = VK_NULL_HANDLE;
    }
    if (m_depthOnlyPipeline) {
        vkDestroyPipeline(m_context->device, m_depthOnlyPipeline, nullptr);
        m_depthOnlyPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_context->device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(m_context->device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool) {
        vkDestroyDescriptorPool(m_context->device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_vertexShader) {
        vkDestroyShaderModule(m_context->device, m_vertexShader, nullptr);
        m_vertexShader = VK_NULL_HANDLE;
    }
    if (m_fragmentShader) {
        vkDestroyShaderModule(m_context->device, m_fragmentShader, nullptr);
        m_fragmentShader = VK_NULL_HANDLE;
    }
    if (m_fragmentShaderSimple) {
        vkDestroyShaderModule(m_context->device, m_fragmentShaderSimple, nullptr);
        m_fragmentShaderSimple = VK_NULL_HANDLE;
    }
    if (m_gbufferRenderPass) {
        vkDestroyRenderPass(m_context->device, m_gbufferRenderPass, nullptr);
        m_gbufferRenderPass = VK_NULL_HANDLE;
    }
    if (m_blockTextureArrayView) {
        vkDestroyImageView(m_context->device, m_blockTextureArrayView, nullptr);
        m_blockTextureArrayView = VK_NULL_HANDLE;
    }
    if (m_blockTextureSampler) {
        vkDestroySampler(m_context->device, m_blockTextureSampler, nullptr);
        m_blockTextureSampler = VK_NULL_HANDLE;
    }
    if (m_blockTextureArray) {
        vmaDestroyImage(m_allocator, m_blockTextureArray, m_blockTextureAllocation);
        m_blockTextureArray = VK_NULL_HANDLE;
        m_blockTextureAllocation = VK_NULL_HANDLE;
    }

    m_unitQuadVertexBuffer.reset();
    m_unitQuadIndexBuffer.reset();
    m_instanceBuffer.reset();
    m_islandTransformBuffer.reset();
    
    m_context = nullptr;  // Mark as cleaned up
}

void VulkanQuadRenderer::detectGPUArchitecture() {
    VkPhysicalDeviceProperties deviceProps;
    vkGetPhysicalDeviceProperties(m_context->physicalDevice, &deviceProps);
    
    // Proper detection: check device type
    m_isIntegratedGPU = (deviceProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    
    // Check if ResizeableBAR/Smart Access Memory is available (discrete GPUs with HOST_VISIBLE DEVICE_LOCAL)
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_context->physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = memProps.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            m_hasHostVisibleDeviceLocal = true;
            break;
        }
    }

    std::cout << "[VulkanQuadRenderer] GPU: " << deviceProps.deviceName << "\n";
    std::cout << "[VulkanQuadRenderer] Type: "
              << (m_isIntegratedGPU ? "Integrated" : "Discrete")
              << (m_hasHostVisibleDeviceLocal ? " (ResizeableBAR available)" : "") << "\n";
}

void VulkanQuadRenderer::createUnitQuad() {
    // Create vertex buffer
    m_unitQuadVertexBuffer = std::make_unique<VulkanBuffer>();
    m_unitQuadVertexBuffer->create(
        m_allocator,
        sizeof(UNIT_QUAD_VERTICES),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0
    );

    // Create index buffer
    m_unitQuadIndexBuffer = std::make_unique<VulkanBuffer>();
    m_unitQuadIndexBuffer->create(
        m_allocator,
        sizeof(UNIT_QUAD_INDICES),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0
    );
}

void VulkanQuadRenderer::uploadUnitQuadData() {
    // Create one-time command buffer
    VkCommandBufferAllocateInfo allocInfo = {}; allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_context->getCommandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(m_context->device, &allocInfo, &cmdBuffer);

    VkCommandBufferBeginInfo beginInfo = {}; beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);

    // Upload vertex data
    vkCmdUpdateBuffer(cmdBuffer, m_unitQuadVertexBuffer->getBuffer(), 0, sizeof(UNIT_QUAD_VERTICES), UNIT_QUAD_VERTICES.data());
    
    // Upload index data
    vkCmdUpdateBuffer(cmdBuffer, m_unitQuadIndexBuffer->getBuffer(), 0, sizeof(UNIT_QUAD_INDICES), UNIT_QUAD_INDICES.data());

    vkEndCommandBuffer(cmdBuffer);

    // Submit and wait (synchronous during initialization)
    VkSubmitInfo submitInfo = {}; submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_context->getGraphicsQueue());
    vkFreeCommandBuffers(m_context->device, m_context->getCommandPool(), 1, &cmdBuffer);
    
    std::cout << "[VulkanQuadRenderer] Unit quad data uploaded\n";
}

// Helper to load SPIR-V from file
static std::vector<uint32_t> loadSPIRV(const std::string& filename) {
    namespace fs = std::filesystem;
    
    // Get executable directory (handles running from different working directories)
    std::string exeDir;
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    fs::path exePath(buffer);
    exeDir = exePath.parent_path().string();
#else
    exeDir = fs::current_path().string();
#endif
    
    // Try exe directory first, then current directory
    std::vector<std::string> searchPaths = {
        exeDir + "/" + filename,
        filename
    };
    
    for (const auto& path : searchPaths) {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t fileSize = (size_t)file.tellg();
            if (fileSize == 0 || fileSize % sizeof(uint32_t) != 0) {
                std::cerr << "[VulkanQuadRenderer] Invalid shader file size: " << fileSize << " bytes\n";
                return {};
            }
            
            std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
            file.seekg(0);
            file.read((char*)buffer.data(), fileSize);
            file.close();
            
            std::cout << "[VulkanQuadRenderer] Loaded shader: " << path << " (" << fileSize << " bytes)\n";
            return buffer;
        }
    }
    
    std::cerr << "[VulkanQuadRenderer] Failed to open shader file: " << filename << "\n";
    std::cerr << "[VulkanQuadRenderer] Searched: exe dir (" << exeDir << ") and current dir\n";
    return {};
}

void VulkanQuadRenderer::createShaders() {
    // Load pre-compiled SPIR-V shaders (vertex pulling version)
    std::vector<uint32_t> vertSpirv = loadSPIRV("shaders/vulkan/quad_vertex_pulling.vert.spv");
    std::vector<uint32_t> fragSpirv = loadSPIRV("shaders/vulkan/quad_gbuffer.frag.spv");  // Use G-buffer shader with textures
    std::vector<uint32_t> fragSimpleSpirv = loadSPIRV("shaders/vulkan/quad_simple.frag.spv");

    if (vertSpirv.empty() || fragSpirv.empty()) {
        std::cerr << "[VulkanQuadRenderer] Failed to load required shaders\n";
        return;
    }

    // Create vertex shader module
    VkShaderModuleCreateInfo vertModuleInfo = {}; vertModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertModuleInfo.codeSize = vertSpirv.size() * sizeof(uint32_t);
    vertModuleInfo.pCode = vertSpirv.data();
    
    if (vkCreateShaderModule(m_context->device, &vertModuleInfo, nullptr, &m_vertexShader) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create vertex shader module\n";
        return;
    }

    // Create fragment shader module (G-buffer)
    VkShaderModuleCreateInfo fragModuleInfo = {}; fragModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragModuleInfo.codeSize = fragSpirv.size() * sizeof(uint32_t);
    fragModuleInfo.pCode = fragSpirv.data();

    if (vkCreateShaderModule(m_context->device, &fragModuleInfo, nullptr, &m_fragmentShader) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create fragment shader module\n";
        return;
    }

    // Create simple fragment shader module (single output for swapchain)
    if (!fragSimpleSpirv.empty()) {
        VkShaderModuleCreateInfo fragSimpleModuleInfo = {}; fragSimpleModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fragSimpleModuleInfo.codeSize = fragSimpleSpirv.size() * sizeof(uint32_t);
        fragSimpleModuleInfo.pCode = fragSimpleSpirv.data();
        
        if (vkCreateShaderModule(m_context->device, &fragSimpleModuleInfo, nullptr, &m_fragmentShaderSimple) != VK_SUCCESS) {
            std::cerr << "[VulkanQuadRenderer] Failed to create simple fragment shader module\n";
        }
    }
    
    std::cout << "[VulkanQuadRenderer] Shaders created successfully\n";
}

void VulkanQuadRenderer::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 3> bindings = {};
    
    // Binding 0: Block Texture Array (fragment shader)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    // Binding 1: Island Transform SSBO (vertex shader)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    
    // Binding 2: Instance Buffer SSBO (vertex shader - vertex pulling)
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {}; layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkResult result = vkCreateDescriptorSetLayout(m_context->device, &layoutInfo, nullptr, &m_descriptorSetLayout);
    if (result != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create descriptor set layout\n";
    }
}

void VulkanQuadRenderer::createPipeline() {
    // Shader stages
    VkPipelineShaderStageCreateInfo vertStage = {}; vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = m_vertexShader;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage = {}; fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = m_fragmentShader;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

    // Vertex pulling: No vertex input state (shader fetches from SSBO)
    VkPipelineVertexInputStateCreateInfo vertexInput = {}; vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 0;
    vertexInput.pVertexBindingDescriptions = nullptr;
    vertexInput.vertexAttributeDescriptionCount = 0;
    vertexInput.pVertexAttributeDescriptions = nullptr;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {}; inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport (dynamic)
    VkPipelineViewportStateCreateInfo viewport = {}; viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer = {}; rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    // Multisampling (disabled)
    VkPipelineMultisampleStateCreateInfo multisampling = {}; multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth/stencil - ENABLED for G-buffer pass
    VkPipelineDepthStencilStateCreateInfo depthStencil = {}; depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Color blend (4 attachments for G-buffer: albedo, normal, position, emissive)
    VkPipelineColorBlendAttachmentState colorBlendAttachments[4] = {};
    for (int i = 0; i < 4; i++) {
        colorBlendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachments[i].blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {}; colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 4;
    colorBlend.pAttachments = colorBlendAttachments;

    // Dynamic state
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {}; dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Push constants (viewProjection matrix + baseQuadIndex offset)
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(glm::mat4) + sizeof(uint32_t);  // mat4 + uint

    // Pipeline layout
    VkPipelineLayoutCreateInfo layoutInfo = {}; layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_context->device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create pipeline layout\n";
        return;
    }

    // Create a temporary render pass matching actual G-buffer layout
    // G-buffer has 4 color attachments (albedo, normal, position, emissive) + depth
    VkAttachmentDescription attachments[5] = {};
    
    // Albedo (R16G16B16A16_SFLOAT to match G-buffer HDR)
    attachments[0].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    // Normal (R16G16B16A16_SFLOAT)
    attachments[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    // Position (R32G32B32A32_SFLOAT to match G-buffer precision)
    attachments[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    // Metadata (R8G8B8A8_UNORM)
    attachments[3].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    // Depth (D32_SFLOAT from VulkanContext)
    attachments[4].format = m_context->getDepthFormat();
    attachments[4].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[4].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[4].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[4].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[4].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[4].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRefs[4] = {
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}
    };
    VkAttachmentReference depthRef = {4, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 4;
    subpass.pColorAttachments = colorRefs;
    subpass.pDepthStencilAttachment = &depthRef;
    
    // Add subpass dependency matching actual G-buffer render pass
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dependencyFlags = 0;

    VkRenderPassCreateInfo rpInfo = {}; rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 5;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;

    vkCreateRenderPass(m_context->device, &rpInfo, nullptr, &m_gbufferRenderPass);

    // Create graphics pipeline with dynamic rendering (G-buffer)
    VkFormat colorFormats[4] = {
        VK_FORMAT_R16G16B16A16_SFLOAT,  // Albedo
        VK_FORMAT_R16G16B16A16_SFLOAT,  // Normal
        VK_FORMAT_R32G32B32A32_SFLOAT,  // Position
        VK_FORMAT_R8G8B8A8_UNORM        // Metadata
    };
    VkFormat depthFormat = m_context->getDepthFormat();  // D32_SFLOAT
    
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = 4;
    renderingInfo.pColorAttachmentFormats = colorFormats;
    renderingInfo.depthAttachmentFormat = depthFormat;
    
    VkGraphicsPipelineCreateInfo pipelineInfo = {}; pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineInfo, nullptr, &m_gbufferPipeline);
    
    if (result != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create graphics pipeline\n";
        return;
    }
}

void VulkanQuadRenderer::createDescriptorPool() {
    std::array<VkDescriptorPoolSize, 2> poolSizes = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 2;  // Island transforms + instance buffer

    VkDescriptorPoolCreateInfo poolInfo = {}; poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;

    vkCreateDescriptorPool(m_context->device, &poolInfo, nullptr, &m_descriptorPool);
}

void VulkanQuadRenderer::updateDescriptorSets() {
    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo = {}; allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    VkResult result = vkAllocateDescriptorSets(m_context->device, &allocInfo, &m_descriptorSet);
    if (result != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] ERROR: Failed to allocate descriptor set, result=" << result << "\n";
        return;
    }

    // Binding 0: Texture array
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_blockTextureArrayView;
    imageInfo.sampler = m_blockTextureSampler;
    
    // Binding 1: Island transforms
    VkDescriptorBufferInfo islandTransformInfo = {};
    islandTransformInfo.buffer = m_islandTransformBuffer->getBuffer();
    islandTransformInfo.offset = 0;
    islandTransformInfo.range = VK_WHOLE_SIZE;
    
    // Binding 2: Instance buffer
    VkDescriptorBufferInfo instanceBufferInfo = {};
    instanceBufferInfo.buffer = m_instanceBuffer->getBuffer();
    instanceBufferInfo.offset = 0;
    instanceBufferInfo.range = VK_WHOLE_SIZE;
    
    std::cout << "[DESCRIPTOR] Instance buffer handle: " << instanceBufferInfo.buffer 
              << ", size: " << (m_instanceBufferCapacity * sizeof(QuadFace)) << " bytes\n";

    std::array<VkWriteDescriptorSet, 3> writes = {};
    
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imageInfo;
    
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &islandTransformInfo;
    
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &instanceBufferInfo;

    vkUpdateDescriptorSets(m_context->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VulkanQuadRenderer::createSwapchainPipeline() {
    // Create a simplified pipeline that renders to swapchain for Phase 2 testing
    // This uses a single color attachment instead of G-buffer
    
    VkPipelineShaderStageCreateInfo vertStage = {}; vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = m_vertexShader;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage = {}; fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = m_fragmentShaderSimple ? m_fragmentShaderSimple : m_fragmentShader;  // Use simple shader if available
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

    // Vertex pulling: No vertex input state (same as G-buffer pipeline)
    VkPipelineVertexInputStateCreateInfo vertexInput = {}; vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 0;
    vertexInput.pVertexBindingDescriptions = nullptr;
    vertexInput.vertexAttributeDescriptionCount = 0;
    vertexInput.pVertexAttributeDescriptions = nullptr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {}; inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport = {}; viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {}; rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;  // CRITICAL: Must be FALSE to rasterize
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {}; multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {}; depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Single color attachment for swapchain
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend = {}; colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {}; dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Create pipeline for swapchain with dynamic rendering
    VkFormat swapchainFormat = m_context->getSwapchainFormat();
    VkFormat depthFormat = m_context->getDepthFormat();
    
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &swapchainFormat;
    renderingInfo.depthAttachmentFormat = depthFormat;
    
    VkGraphicsPipelineCreateInfo pipelineInfo = {}; pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    
    VkResult result = vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineInfo, nullptr, &m_swapchainPipeline);
    if (result != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create swapchain pipeline, error=" << result << "\n";
        return;
    }
}

void VulkanQuadRenderer::createDepthOnlyPipeline() {
    // Depth-only pipeline for shadow map rendering (Phase 4)
    // No fragment shader needed - only write depth
    
    VkPipelineShaderStageCreateInfo vertStage = {}; 
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = m_vertexShader;
    vertStage.pName = "main";

    // Vertex pulling: No vertex input state
    VkPipelineVertexInputStateCreateInfo vertexInput = {}; 
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {}; 
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport = {}; 
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {}; 
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;  // Enable depth bias for shadow maps
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {}; 
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {}; 
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // No color attachments for depth-only
    VkPipelineColorBlendStateCreateInfo colorBlend = {}; 
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 0;

    VkDynamicState dynamicStates[] = { 
        VK_DYNAMIC_STATE_VIEWPORT, 
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS  // Dynamic depth bias
    };
    VkPipelineDynamicStateCreateInfo dynamicState = {}; 
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 3;
    dynamicState.pDynamicStates = dynamicStates;

    // PLACEHOLDER - actual pipeline created in renderDepthOnly when needed
    // This avoids initialization order issues with shadow map render pass
}

void VulkanQuadRenderer::ensureDepthPipeline() {
    if (m_depthOnlyPipeline) return;
    
    VkPipelineShaderStageCreateInfo vertStage = {}; 
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = m_vertexShader;
    vertStage.pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput = {}; 
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {}; 
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport = {}; 
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {}; 
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {}; 
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {}; 
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo colorBlend = {}; 
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 0;

    VkDynamicState dynamicStates[] = { 
        VK_DYNAMIC_STATE_VIEWPORT, 
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS
    };
    VkPipelineDynamicStateCreateInfo dynamicState = {}; 
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 3;
    dynamicState.pDynamicStates = dynamicStates;

    // Dynamic rendering: specify depth format instead of render pass
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo pipelineInfo = {}; 
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 1;
    pipelineInfo.pStages = &vertStage;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    
    VkResult result = vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineInfo, nullptr, &m_depthOnlyPipeline);
    if (result != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create depth-only pipeline\n";
    }
}

void VulkanQuadRenderer::renderDepthOnly(VkCommandBuffer cmd, const glm::mat4& lightViewProjection) {
    if (m_chunks.empty()) return;
    
    ensureDepthPipeline();
    if (!m_depthOnlyPipeline) return;  // Pipeline creation failed

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_depthOnlyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    
    // Push light view-projection matrix
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &lightViewProjection);
    
    // Set depth bias to avoid shadow acne
    vkCmdSetDepthBias(cmd, 1.25f, 0.0f, 1.75f);

    // Draw all chunks
    for (const auto& chunk : m_chunks) {
        if (chunk.instanceCount == 0) continue;
        
        uint32_t vertexCount = chunk.instanceCount * 6;
        uint32_t baseQuadIndex = chunk.baseInstance;
        
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), sizeof(uint32_t), &baseQuadIndex);
        vkCmdDraw(cmd, vertexCount, 1, 0, 0);
    }
}

void VulkanQuadRenderer::updateDynamicBuffers(VkCommandBuffer cmd, const glm::mat4& /*viewProjection*/) {
    if (m_chunks.empty()) return;

    // Update island transform buffer using vkCmdUpdateBuffer (must be called OUTSIDE render pass)
    if (!m_islandTransforms.empty()) {
        // Build contiguous array of transforms indexed by islandID
        std::vector<glm::mat4> transforms(1024, glm::mat4(1.0f));  // Max 1024 islands, default identity
        for (const auto& [islandID, transform] : m_islandTransforms) {
            if (islandID < transforms.size()) {
                transforms[islandID] = transform;
            }
        }
        
        size_t dataSize = transforms.size() * sizeof(glm::mat4);
        // vkCmdUpdateBuffer max size is 65536 bytes, split if needed
        size_t offset = 0;
        const size_t maxUpdateSize = 65536;
        while (offset < dataSize) {
            size_t updateSize = std::min(dataSize - offset, maxUpdateSize);
            vkCmdUpdateBuffer(cmd, m_islandTransformBuffer->getBuffer(), offset, 
                             updateSize, reinterpret_cast<const char*>(transforms.data()) + offset);
            offset += updateSize;
        }
        
        // Barrier: Transfer writes -> Shader reads
        VkBufferMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = m_islandTransformBuffer->getBuffer();
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
        
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                            0, 0, nullptr, 1, &barrier, 0, nullptr);
    }
}

void VulkanQuadRenderer::renderToSwapchain(VkCommandBuffer cmd, const glm::mat4& viewProjection, const glm::mat4& /*view*/) {
    if (!m_swapchainPipeline) return;
    if (m_chunks.empty()) return;

    // Bind swapchain pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_swapchainPipeline);

    // Set dynamic viewport and scissor (Vulkan Y-axis points down)
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_context->getSwapchainExtent().width);
    viewport.height = static_cast<float>(m_context->getSwapchainExtent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_context->getSwapchainExtent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Push viewProjection once (applies to all chunks)
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &viewProjection);

    // Draw each chunk with its baseQuadIndex offset in push constants
    for (const auto& chunk : m_chunks) {
        if (chunk.instanceCount == 0) continue;
        
        uint32_t vertexCount = chunk.instanceCount * 6;
        uint32_t baseQuadIndex = chunk.baseInstance;
        
        // Push baseQuadIndex for this chunk (offset after viewProjection matrix)
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 
                          sizeof(glm::mat4), sizeof(uint32_t), &baseQuadIndex);
        
        vkCmdDraw(cmd, vertexCount, 1, 0, 0);
    }
}

void VulkanQuadRenderer::registerChunk(VoxelChunk* chunk, uint32_t islandID, const glm::vec3& chunkOffset) {
    if (m_chunkToIndex.find(chunk) != m_chunkToIndex.end()) {
        return;  // Already registered
    }

    ChunkEntry entry;
    entry.chunk = chunk;
    entry.islandID = islandID;
    entry.chunkOffset = chunkOffset;
    entry.instanceCount = 0;
    entry.baseInstance = 0;  // Will be set in uploadInstanceData when mesh is uploaded
    entry.allocatedSlots = 0;  // Will be set in uploadInstanceData when actual quad count is known
    entry.needsGPUSync = false;

    m_chunks.push_back(entry);
    m_chunkToIndex[chunk] = m_chunks.size() - 1;
    
    // Track unique island IDs
    if (m_islandTransforms.find(islandID) == m_islandTransforms.end()) {
        m_islandTransforms[islandID] = glm::mat4(1.0f);  // Identity default
        m_islandIDList.push_back(islandID);
    }
}

void VulkanQuadRenderer::updateIslandTransform(uint32_t islandID, const glm::mat4& transform) {
    auto it = m_islandTransforms.find(islandID);
    if (it != m_islandTransforms.end()) {
        it->second = transform;
    } else {
        // New island - add to tracking
        m_islandTransforms[islandID] = transform;
        m_islandIDList.push_back(islandID);
    }
}

void VulkanQuadRenderer::uploadChunkMesh(VoxelChunk* chunk) {
    auto it = m_chunkToIndex.find(chunk);
    if (it == m_chunkToIndex.end()) return;
    
    // Update allocation and prepare data
    uploadInstanceData(m_chunks[it->second]);
    
    // Queue for batched upload
    m_pendingUploads.push_back(&m_chunks[it->second]);
}void VulkanQuadRenderer::uploadInstanceData(ChunkEntry& entry) {
    auto mesh = entry.chunk->getRenderMesh();
    if (!mesh) {
        return;
    }
    if (mesh->quads.empty()) {
        return;
    }

    size_t newQuadCount = mesh->quads.size();
    
    // First upload or need more space
    if (newQuadCount > entry.allocatedSlots) {
        // CRITICAL: Only allow initial allocation, not reallocation
        // Reallocation would move baseInstance, breaking all existing draw calls
        if (entry.allocatedSlots > 0) {
            // Mesh grew beyond capacity - clamp to allocated space
            // The 25% padding on initial allocation should handle most cases
            // If this still happens frequently, the async meshing system will
            // eventually re-merge quads with greedy meshing
            newQuadCount = entry.allocatedSlots;
        } else {
            // First allocation: Add 25% padding for block breaking (greedy mesh explosion)
            // Round up to nearest 256 for alignment
            size_t withPadding = newQuadCount + (newQuadCount / 4);
            size_t newAllocation = ((withPadding + 255) / 256) * 256;
            
            // Ensure minimum allocation of 256 quads
            if (newAllocation < 256) newAllocation = 256;
            
            if (m_instanceBufferUsed + newAllocation > m_instanceBufferCapacity) {
                std::cerr << "[VulkanQuadRenderer] Instance buffer overflow!\n";
                return;
            }
            
            entry.baseInstance = m_instanceBufferUsed;
            entry.allocatedSlots = newAllocation;
            
            m_instanceBufferUsed += newAllocation;
        }
    }
    
    // Populate island IDs in quad data (convert chunk-relative to island-relative positions)
    std::vector<QuadFace> quadsWithIslandID = mesh->quads;
    
    for (auto& quad : quadsWithIslandID) {
        quad.islandID = entry.islandID;
        // Position is already chunk-relative, add chunk offset for island-relative coords
        quad.position += entry.chunkOffset;
    }
    
    entry.instanceCount = newQuadCount;
    entry.needsGPUSync = true;
    
    // Upload is deferred to processPendingUploads() for batching (anti-pattern: per-chunk command buffers)
}

void VulkanQuadRenderer::processPendingUploads() {
    if (m_pendingUploads.empty()) return;
    
    // Allocate single command buffer for all uploads
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_context->getCommandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(m_context->device, &allocInfo, &cmdBuffer);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    
    // Batch all uploads into single command buffer
    for (ChunkEntry* entry : m_pendingUploads) {
        auto mesh = entry->chunk->getRenderMesh();
        if (!mesh || mesh->quads.empty()) continue;
        
        size_t newQuadCount = mesh->quads.size();
        
        // Allocation already done in updateChunkMesh() - just verify
        if (newQuadCount > entry->allocatedSlots) {
            std::cerr << "[VulkanQuadRenderer] ERROR: Chunk mesh grew beyond initial allocation!\n";
            newQuadCount = entry->allocatedSlots;
        }
        
        // Populate island IDs
        std::vector<QuadFace> quadsWithIslandID = mesh->quads;
        for (auto& quad : quadsWithIslandID) {
            quad.islandID = entry->islandID;
            quad.position += entry->chunkOffset;
        }
        
        entry->instanceCount = newQuadCount;
        size_t uploadSize = newQuadCount * sizeof(QuadFace);
        size_t uploadOffset = entry->baseInstance * sizeof(QuadFace);
        
        // Add upload commands to batch
        const size_t maxUpdateSize = (65536 / sizeof(QuadFace)) * sizeof(QuadFace);
        size_t offset = 0;
        while (offset < uploadSize) {
            size_t updateSize = std::min(uploadSize - offset, maxUpdateSize);
            vkCmdUpdateBuffer(cmdBuffer, m_instanceBuffer->getBuffer(), 
                             uploadOffset + offset, updateSize,
                             reinterpret_cast<const char*>(quadsWithIslandID.data()) + offset);
            offset += updateSize;
        }
        
        entry->needsGPUSync = true;
    }
    
    // Single barrier for entire batch (not per-chunk - wasteful)
    VkBufferMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = m_instanceBuffer->getBuffer();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                        0, 0, nullptr, 1, &barrier, 0, nullptr);
    
    vkEndCommandBuffer(cmdBuffer);
    
    // Submit and wait (synchronous during initialization)
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_context->getGraphicsQueue());
    vkFreeCommandBuffers(m_context->device, m_context->getCommandPool(), 1, &cmdBuffer);
    
    m_pendingUploads.clear();
}

void VulkanQuadRenderer::renderToGBuffer(VkCommandBuffer cmd, const glm::mat4& viewProjection, const glm::mat4& /*view*/) {
    if (m_chunks.empty()) return;

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);

    // Bind descriptor set (transform buffer)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Bind vertex buffers: binding 0 = unit quad vertices, binding 1 = instance data
    VkBuffer vertexBuffers[] = { m_unitQuadVertexBuffer->getBuffer(), m_instanceBuffer->getBuffer() };
    VkDeviceSize offsets[] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);

    // Bind index buffer
    vkCmdBindIndexBuffer(cmd, m_unitQuadIndexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);

    // Direct draw for each chunk (vertex pulling with push constants)
    struct PushConstants {
        glm::mat4 viewProjection;
        uint32_t baseQuadIndex;
    };
    
    for (size_t i = 0; i < m_chunks.size(); i++) {
        if (m_chunks[i].instanceCount == 0) continue;
        
        // Update push constants per chunk
        PushConstants pushConst;
        pushConst.viewProjection = viewProjection;
        pushConst.baseQuadIndex = m_chunks[i].baseInstance;
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &pushConst);
        
        uint32_t vertexCount = 6 * m_chunks[i].instanceCount;  // 6 vertices per quad
        vkCmdDraw(cmd, vertexCount, 1, 0, 0);
    }
}

void VulkanQuadRenderer::clear() {
    m_chunks.clear();
    m_chunkToIndex.clear();
    m_islandTransforms.clear();
    m_islandIDList.clear();
    m_instanceBufferUsed = 0;
}

bool VulkanQuadRenderer::loadBlockTextureArray() {
    std::cout << "[VulkanQuadRenderer] Loading block texture array...\n";
    
    auto& blockRegistry = BlockTypeRegistry::getInstance();
    const auto& blockTypes = blockRegistry.getAllBlockTypes();
    
    // Find asset path from exe directory
    std::filesystem::path exePath = std::filesystem::current_path();
    std::filesystem::path texturePath = exePath / "assets" / "textures";
    
    if (!std::filesystem::exists(texturePath)) {
        std::cerr << "[VulkanQuadRenderer] Texture directory not found: " << texturePath << "\n";
        return createPlaceholderTexture();
    }
    
    // Load all block textures
    std::vector<unsigned char*> textureData;
    std::vector<int> textureWidths, textureHeights;
    int commonWidth = 0, commonHeight = 0;
    
    // Reserve space for MAX_BLOCK_TYPES textures
    textureData.resize(BlockID::MAX_BLOCK_TYPES, nullptr);
    textureWidths.resize(BlockID::MAX_BLOCK_TYPES, 0);
    textureHeights.resize(BlockID::MAX_BLOCK_TYPES, 0);
    
    int loadedCount = 0;
    for (const auto& blockType : blockTypes) {
        if (blockType.renderType != BlockRenderType::VOXEL) continue;
        if (blockType.id == BlockID::AIR) continue;  // Skip air
        
        // Construct texture filename from block name
        std::string textureName = blockType.name + ".png";
        std::filesystem::path textureFile = texturePath / textureName;
        
        if (!std::filesystem::exists(textureFile)) {
            std::cerr << "[VulkanQuadRenderer] Missing texture: " << textureFile << "\n";
            continue;
        }
        
        int width, height, channels;
        unsigned char* pixels = stbi_load(textureFile.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
        
        if (!pixels) {
            std::cerr << "[VulkanQuadRenderer] Failed to load texture: " << textureFile << "\n";
            continue;
        }
        
        // Validate size consistency
        if (loadedCount == 0) {
            commonWidth = width;
            commonHeight = height;
        } else if (width != commonWidth || height != commonHeight) {
            std::cerr << "[VulkanQuadRenderer] Texture size mismatch: " << textureFile 
                      << " is " << width << "x" << height << ", expected " 
                      << commonWidth << "x" << commonHeight << "\n";
            stbi_image_free(pixels);
            continue;
        }
        
        textureData[blockType.id] = pixels;
        textureWidths[blockType.id] = width;
        textureHeights[blockType.id] = height;
        loadedCount++;
    }
    
    if (loadedCount == 0) {
        std::cerr << "[VulkanQuadRenderer] No textures loaded!\n";
        return createPlaceholderTexture();
    }
    
    std::cout << "[VulkanQuadRenderer] Loaded " << loadedCount << " textures (" 
              << commonWidth << "x" << commonHeight << ")\n";
    
    // Create Vulkan texture array
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.extent.width = commonWidth;
    imageInfo.extent.height = commonHeight;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = BlockID::MAX_BLOCK_TYPES;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    
    if (vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &m_blockTextureArray, 
                       &m_blockTextureAllocation, nullptr) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create texture array image\n";
        for (auto* data : textureData) {
            if (data) stbi_image_free(data);
        }
        return false;
    }
    
    // Upload texture data
    VkDeviceSize layerSize = commonWidth * commonHeight * 4;
    VkDeviceSize totalSize = layerSize * BlockID::MAX_BLOCK_TYPES;
    
    // CRITICAL GPU ARCHITECTURE CONSTRAINT:
    // NVIDIA RTX 3070 Ti Laptop restricts TRANSFER_SRC buffers to memoryTypeBits=0x7 (types 0,1,2)
    // Type 5 (ResizableBAR) bit = 0x20, which is NOT in 0x7 mask
    // CONCLUSION: Traditional staging buffers are IMPOSSIBLE on this GPU architecture
    // SOLUTION: Create GPU-only buffer + use vkCmdUpdateBuffer to populate it
    
    VkBuffer transferBuffer;
    VmaAllocation transferAllocation;
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = totalSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    
    VmaAllocationCreateInfo transferAllocInfo = {};
    transferAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;  // Device-local only
    
    VkResult result = vmaCreateBuffer(m_allocator, &bufferInfo, &transferAllocInfo, 
                        &transferBuffer, &transferAllocation, nullptr);
    if (result != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create transfer buffer (error: " << result << ")\n";
        vmaDestroyImage(m_allocator, m_blockTextureArray, m_blockTextureAllocation);
        for (auto* data : textureData) {
            if (data) stbi_image_free(data);
        }
        return createPlaceholderTexture();
    }
    
    std::cout << "[VulkanQuadRenderer] Created GPU-only transfer buffer (" << totalSize / 1024 / 1024 << " MB)\n";
    
    // Build texture data in CPU memory
    std::vector<unsigned char> cpuTextureData(totalSize);
    for (size_t i = 0; i < BlockID::MAX_BLOCK_TYPES; i++) {
        unsigned char* dest = cpuTextureData.data() + i * layerSize;
        if (textureData[i]) {
            memcpy(dest, textureData[i], layerSize);
        } else {
            // Fill with magenta (missing texture indicator)
            for (size_t pixel = 0; pixel < layerSize / 4; pixel++) {
                size_t offset = pixel * 4;
                dest[offset + 0] = 255;  // R
                dest[offset + 1] = 0;    // G
                dest[offset + 2] = 255;  // B
                dest[offset + 3] = 255;  // A
            }
        }
    }
    
    // Free stb_image data
    for (auto* data : textureData) {
        if (data) stbi_image_free(data);
    }
    
    // Create command buffer for upload
    VkCommandBufferAllocateInfo cmdAllocInfo = {};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = m_context->getCommandPool();
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(m_context->device, &cmdAllocInfo, &cmdBuffer);
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    
    // Upload texture data using vkCmdUpdateBuffer (65KB chunks)
    // This is the ONLY way to upload to GPU-only buffers on RTX 3070 Ti Laptop
    const VkDeviceSize maxUpdateSize = 65536;
    VkDeviceSize offset = 0;
    std::cout << "[VulkanQuadRenderer] Uploading " << totalSize / 1024 << " KB in " 
              << ((totalSize + maxUpdateSize - 1) / maxUpdateSize) << " chunks...\n";
    
    while (offset < totalSize) {
        VkDeviceSize updateSize = std::min(totalSize - offset, maxUpdateSize);
        vkCmdUpdateBuffer(cmdBuffer, transferBuffer, offset, updateSize, 
                         cpuTextureData.data() + offset);
        offset += updateSize;
    }
    
    // Barrier: Transfer write → Transfer read
    VkBufferMemoryBarrier bufferBarrier = {};
    bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bufferBarrier.buffer = transferBuffer;
    bufferBarrier.offset = 0;
    bufferBarrier.size = VK_WHOLE_SIZE;
    
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 
                         0, nullptr, 1, &bufferBarrier, 0, nullptr);
    
    // Transition image to TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier imageBarrier = {};
    imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = m_blockTextureArray;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.baseMipLevel = 0;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.baseArrayLayer = 0;
    imageBarrier.subresourceRange.layerCount = BlockID::MAX_BLOCK_TYPES;
    imageBarrier.srcAccessMask = 0;
    imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 
                         0, nullptr, 0, nullptr, 1, &imageBarrier);
    
    // Copy buffer to image
    std::vector<VkBufferImageCopy> copyRegions;
    for (uint32_t layer = 0; layer < BlockID::MAX_BLOCK_TYPES; layer++) {
        VkBufferImageCopy region = {};
        region.bufferOffset = layer * layerSize;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {(uint32_t)commonWidth, (uint32_t)commonHeight, 1};
        copyRegions.push_back(region);
    }
    
    vkCmdCopyBufferToImage(cmdBuffer, transferBuffer, m_blockTextureArray, 
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 
                           copyRegions.size(), copyRegions.data());
    
    // Transition image to SHADER_READ_ONLY_OPTIMAL
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 
                         0, nullptr, 0, nullptr, 1, &imageBarrier);
    
    vkEndCommandBuffer(cmdBuffer);
    
    // Submit and wait (synchronous during initialization)
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    
    vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_context->getGraphicsQueue());
    vkFreeCommandBuffers(m_context->device, m_context->getCommandPool(), 1, &cmdBuffer);
    vmaDestroyBuffer(m_allocator, transferBuffer, transferAllocation);
    
    // Create image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_blockTextureArray;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = BlockID::MAX_BLOCK_TYPES;
    
    if (vkCreateImageView(m_context->device, &viewInfo, nullptr, &m_blockTextureArrayView) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create texture array image view\n";
        return false;
    }
    
    // Create sampler
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;  // Pixel art style
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.maxLod = 1.0f;
    
    if (vkCreateSampler(m_context->device, &samplerInfo, nullptr, &m_blockTextureSampler) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create texture sampler\n";
        return false;
    }
    
    std::cout << "[VulkanQuadRenderer] Block texture array created successfully\n";
    return true;
}

bool VulkanQuadRenderer::createPlaceholderTexture() {
    std::cout << "[VulkanQuadRenderer] Creating placeholder texture array (1x1 magenta)...\n";
    
    // CRITICAL: Use 1x1 texture to stay under vkCmdUpdateBuffer 65KB limit
    // 1x1x4 bytes = 4 bytes per layer * 104 layers = 416 bytes total (well under 65KB)
    const int width = 1;
    const int height = 1;
    const int channels = 4;
    const VkDeviceSize layerSize = width * height * channels;
    const VkDeviceSize totalSize = layerSize * BlockID::MAX_BLOCK_TYPES;
    
    // Create magenta pixels (missing texture indicator)
    std::vector<unsigned char> pixels(totalSize);
    for (uint32_t layer = 0; layer < BlockID::MAX_BLOCK_TYPES; layer++) {
        size_t offset = layer * layerSize;
        pixels[offset + 0] = 255;  // R
        pixels[offset + 1] = 0;    // G
        pixels[offset + 2] = 255;  // B
        pixels[offset + 3] = 255;  // A
    }
    
    // Create image (same as loadBlockTextureArray but with placeholder data)
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = BlockID::MAX_BLOCK_TYPES;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    
    if (vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &m_blockTextureArray, 
                       &m_blockTextureAllocation, nullptr) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create placeholder image\n";
        return false;
    }
    
    // Use vkCmdClearColorImage - no staging buffer needed
    VkCommandBufferAllocateInfo cmdAllocInfo = {};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = m_context->getCommandPool();
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(m_context->device, &cmdAllocInfo, &cmdBuffer);
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    
    // Transition to TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_blockTextureArray;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = BlockID::MAX_BLOCK_TYPES;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Clear all layers to magenta
    VkClearColorValue clearColor = {};
    clearColor.float32[0] = 1.0f;  // R
    clearColor.float32[1] = 0.0f;  // G
    clearColor.float32[2] = 1.0f;  // B
    clearColor.float32[3] = 1.0f;  // A
    
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = BlockID::MAX_BLOCK_TYPES;
    
    vkCmdClearColorImage(cmdBuffer, m_blockTextureArray, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);
    
    // Transition to SHADER_READ_ONLY_OPTIMAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    vkEndCommandBuffer(cmdBuffer);
    
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    
    vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_context->getGraphicsQueue());
    
    vkFreeCommandBuffers(m_context->device, m_context->getCommandPool(), 1, &cmdBuffer);
    
    // Create image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_blockTextureArray;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = BlockID::MAX_BLOCK_TYPES;
    
    if (vkCreateImageView(m_context->device, &viewInfo, nullptr, &m_blockTextureArrayView) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create placeholder image view\n";
        return false;
    }
    
    // Create sampler
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    
    if (vkCreateSampler(m_context->device, &samplerInfo, nullptr, &m_blockTextureSampler) != VK_SUCCESS) {
        std::cerr << "[VulkanQuadRenderer] Failed to create placeholder sampler\n";
        return false;
    }
    
    std::cout << "[VulkanQuadRenderer] Placeholder texture created successfully (1x1 magenta)\n";
    return true;
}
