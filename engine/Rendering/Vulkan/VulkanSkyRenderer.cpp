// VulkanSkyRenderer.cpp - Vulkan port of SkyRenderer
#include "VulkanSkyRenderer.h"
#include "VulkanContext.h"
#include "VulkanBuffer.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

VulkanSkyRenderer::VulkanSkyRenderer() {}

VulkanSkyRenderer::~VulkanSkyRenderer() {
    shutdown();
}

bool VulkanSkyRenderer::initialize(VulkanContext* ctx) {
    if (m_initialized) return true;
    if (!ctx) return false;

    m_context = ctx;
    m_allocator = ctx->getAllocator();

    std::cout << "🌅 Initializing Vulkan Sky Renderer..." << std::endl;

    if (!createShaders()) {
        std::cerr << "❌ Failed to create sky shaders" << std::endl;
        return false;
    }

    if (!createGeometry()) {
        std::cerr << "❌ Failed to create sky geometry" << std::endl;
        return false;
    }

    if (!createPipeline()) {
        std::cerr << "❌ Failed to create sky pipeline" << std::endl;
        return false;
    }

    m_initialized = true;
    std::cout << "   └─ Vulkan sky renderer initialized successfully" << std::endl;
    return true;
}

void VulkanSkyRenderer::shutdown() {
    if (!m_context) return;

    VkDevice device = m_context->getDevice();

    if (m_pipeline) vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_vertexShader) vkDestroyShaderModule(device, m_vertexShader, nullptr);
    if (m_fragmentShader) vkDestroyShaderModule(device, m_fragmentShader, nullptr);

    m_vertexBuffer.reset();
    m_indexBuffer.reset();

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_vertexShader = VK_NULL_HANDLE;
    m_fragmentShader = VK_NULL_HANDLE;

    m_initialized = false;
}

bool VulkanSkyRenderer::createShaders() {
    // Load compiled SPIR-V shaders
    // Get exe directory for shader loading
    std::string exeDir;
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::filesystem::path exePath(buffer);
    exeDir = exePath.parent_path().string();
#else
    exeDir = std::filesystem::current_path().string();
#endif

    std::string vertPath = exeDir + "/shaders/vulkan/sky.vert.spv";
    std::string fragPath = exeDir + "/shaders/vulkan/sky.frag.spv";
    
    std::ifstream vertFile(vertPath, std::ios::binary | std::ios::ate);
    std::ifstream fragFile(fragPath, std::ios::binary | std::ios::ate);

    if (!vertFile.is_open()) {
        std::cerr << "❌ Failed to open " << vertPath << std::endl;
        return false;
    }
    if (!fragFile.is_open()) {
        std::cerr << "❌ Failed to open " << fragPath << std::endl;
        return false;
    }

    size_t vertSize = vertFile.tellg();
    size_t fragSize = fragFile.tellg();
    vertFile.seekg(0);
    fragFile.seekg(0);

    std::vector<char> vertCode(vertSize);
    std::vector<char> fragCode(fragSize);
    vertFile.read(vertCode.data(), vertSize);
    fragFile.read(fragCode.data(), fragSize);

    VkDevice device = m_context->getDevice();

    // Create vertex shader module
    VkShaderModuleCreateInfo vertCreateInfo{};
    vertCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertCreateInfo.codeSize = vertCode.size();
    vertCreateInfo.pCode = reinterpret_cast<const uint32_t*>(vertCode.data());

    if (vkCreateShaderModule(device, &vertCreateInfo, nullptr, &m_vertexShader) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create vertex shader module" << std::endl;
        return false;
    }

    // Create fragment shader module
    VkShaderModuleCreateInfo fragCreateInfo{};
    fragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragCreateInfo.codeSize = fragCode.size();
    fragCreateInfo.pCode = reinterpret_cast<const uint32_t*>(fragCode.data());

    if (vkCreateShaderModule(device, &fragCreateInfo, nullptr, &m_fragmentShader) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create fragment shader module" << std::endl;
        return false;
    }

    return true;
}

bool VulkanSkyRenderer::createGeometry() {
    // Skybox cube vertices (unit cube centered at origin)
    float vertices[] = {
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f
    };

    // Cube indices (36 indices for 12 triangles)
    uint16_t indices[] = {
        0, 1, 2, 2, 3, 0, // Front
        1, 5, 6, 6, 2, 1, // Right
        5, 4, 7, 7, 6, 5, // Back
        4, 0, 3, 3, 7, 4, // Left
        3, 2, 6, 6, 7, 3, // Top
        4, 5, 1, 1, 0, 4  // Bottom
    };

    // Create vertex buffer (GPU_ONLY with TRANSFER_DST for vkCmdUpdateBuffer upload)
    m_vertexBuffer = std::make_unique<VulkanBuffer>();
    if (!m_vertexBuffer->create(m_allocator, sizeof(vertices), 
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VMA_MEMORY_USAGE_GPU_ONLY,
                                0)) {
        std::cerr << "❌ Failed to create vertex buffer" << std::endl;
        return false;
    }

    // Create index buffer (GPU_ONLY with TRANSFER_DST for vkCmdUpdateBuffer upload)
    m_indexBuffer = std::make_unique<VulkanBuffer>();
    if (!m_indexBuffer->create(m_allocator, sizeof(indices),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VMA_MEMORY_USAGE_GPU_ONLY,
                               0)) {
        std::cerr << "❌ Failed to create index buffer" << std::endl;
        return false;
    }

    // Upload via vkCmdUpdateBuffer (like VulkanQuadRenderer does for small static data)
    VkCommandBuffer cmdBuffer = m_context->beginSingleTimeCommands();
    vkCmdUpdateBuffer(cmdBuffer, m_vertexBuffer->getBuffer(), 0, sizeof(vertices), vertices);
    vkCmdUpdateBuffer(cmdBuffer, m_indexBuffer->getBuffer(), 0, sizeof(indices), indices);
    m_context->endSingleTimeCommands(cmdBuffer);

    std::cout << "✅ Sky geometry created (GPU_ONLY + vkCmdUpdateBuffer)" << std::endl;
    return true;
}

bool VulkanSkyRenderer::createPipeline() {
    VkDevice device = m_context->getDevice();

    // Push constant range for all uniforms
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    // Pipeline layout
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create pipeline layout" << std::endl;
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = m_vertexShader;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = m_fragmentShader;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    // Vertex input (vec3 position only)
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = 3 * sizeof(float);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributeDescription{};
    attributeDescription.binding = 0;
    attributeDescription.location = 0;
    attributeDescription.format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescription.offset = 0;

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 1;
    vertexInputInfo.pVertexAttributeDescriptions = &attributeDescription;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // No culling for skybox
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth stencil (render at far plane, don't write depth)
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE; // Don't write to depth
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // Accept pixels at far plane
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Color blending (no blending, replace background)
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Dynamic state
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_context->getRenderPass();
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device, m_context->pipelineCache, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create graphics pipeline" << std::endl;
        return false;
    }

    return true;
}

void VulkanSkyRenderer::render(VkCommandBuffer cmd,
                               const glm::vec3& sunDirection, float sunIntensity,
                               const glm::vec3& moonDirection, float moonIntensity,
                               const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                               float timeOfDay) {
    if (!m_initialized) return;

    // Remove translation from view matrix for skybox
    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(viewMatrix));
    glm::mat4 viewProj = projectionMatrix * viewNoTranslation;

    // Fill push constants
    PushConstants pushConstants{};
    pushConstants.viewProj = viewProj;
    pushConstants.sunDir = sunDirection;
    pushConstants.sunIntensity = sunIntensity;
    pushConstants.moonDir = moonDirection;
    pushConstants.moonIntensity = moonIntensity;
    pushConstants.cameraPos = glm::vec3(0.0f); // Not used in skybox
    pushConstants.timeOfDay = timeOfDay;
    pushConstants.sunSize = m_sunSize;
    pushConstants.sunGlow = m_sunGlow;
    pushConstants.moonSize = m_moonSize;
    pushConstants.exposure = m_exposure;

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Set dynamic viewport and scissor (required by pipeline dynamic state)
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

    // Push constants
    vkCmdPushConstants(cmd, m_pipelineLayout, 
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, sizeof(PushConstants), &pushConstants);

    // Bind vertex buffer
    VkDeviceSize offsets[] = {0};
    VkBuffer vertexBuffer = m_vertexBuffer->getBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, offsets);

    // Bind index buffer
    VkBuffer indexBuffer = m_indexBuffer->getBuffer();
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT16);

    // Draw skybox
    vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
}
