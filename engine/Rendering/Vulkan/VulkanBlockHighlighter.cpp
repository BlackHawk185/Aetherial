// VulkanBlockHighlighter.cpp - Vulkan wireframe cube renderer
#include "VulkanBlockHighlighter.h"
#include "VulkanContext.h"
#include "../../Math/Vec3.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <array>

VulkanBlockHighlighter::VulkanBlockHighlighter() {}

VulkanBlockHighlighter::~VulkanBlockHighlighter() {
    shutdown();
}

bool VulkanBlockHighlighter::initialize(VulkanContext* ctx) {
    m_context = ctx;
    m_allocator = ctx->allocator;
    
    if (!createBuffers()) return false;
    if (!createShaders()) return false;
    if (!createPipeline()) return false;
    
    return true;
}

void VulkanBlockHighlighter::shutdown() {
    if (!m_context || !m_context->device) return;
    
    // Wait for GPU to finish using buffers before destroying them
    vkDeviceWaitIdle(m_context->device);
    
    if (m_pipeline) {
        vkDestroyPipeline(m_context->device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_context->device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_vertexShader) {
        vkDestroyShaderModule(m_context->device, m_vertexShader, nullptr);
        m_vertexShader = VK_NULL_HANDLE;
    }
    if (m_fragmentShader) {
        vkDestroyShaderModule(m_context->device, m_fragmentShader, nullptr);
        m_fragmentShader = VK_NULL_HANDLE;
    }
    
    m_vertexBuffer.reset();
    m_indexBuffer.reset();
}

bool VulkanBlockHighlighter::createBuffers() {
    // Cube vertices (slightly enlarged)
    float offset = 0.501f;
    std::array<glm::vec3, 8> vertices = {{
        {-offset, -offset, -offset},  // 0
        { offset, -offset, -offset},  // 1
        { offset,  offset, -offset},  // 2
        {-offset,  offset, -offset},  // 3
        {-offset, -offset,  offset},  // 4
        { offset, -offset,  offset},  // 5
        { offset,  offset,  offset},  // 6
        {-offset,  offset,  offset}   // 7
    }};
    
    // Line indices (12 edges)
    std::array<uint16_t, 24> indices = {{
        0, 1,  1, 2,  2, 3,  3, 0,  // Front face
        4, 5,  5, 6,  6, 7,  7, 4,  // Back face
        0, 4,  1, 5,  2, 6,  3, 7   // Connecting edges
    }};
    
    // Create vertex buffer (GPU-only with TRANSFER_DST for vkCmdUpdateBuffer)
    m_vertexBuffer = std::make_unique<VulkanBuffer>();
    if (!m_vertexBuffer->create(m_allocator, sizeof(vertices), 
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0)) {
        std::cerr << "[VulkanBlockHighlighter] Failed to create vertex buffer\n";
        return false;
    }
    
    // Create index buffer (GPU-only with TRANSFER_DST for vkCmdUpdateBuffer)
    m_indexBuffer = std::make_unique<VulkanBuffer>();
    if (!m_indexBuffer->create(m_allocator, sizeof(indices),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0)) {
        std::cerr << "[VulkanBlockHighlighter] Failed to create index buffer\n";
        return false;
    }
    
    // Upload data using vkCmdUpdateBuffer (no staging buffers - GPU architecture constraint)
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();
    
    // Upload vertices and indices (both under 65KB limit)
    vkCmdUpdateBuffer(cmd, m_vertexBuffer->getBuffer(), 0, sizeof(vertices), vertices.data());
    vkCmdUpdateBuffer(cmd, m_indexBuffer->getBuffer(), 0, sizeof(indices), indices.data());
    
    // Barrier: Transfer write → Vertex input read
    VkBufferMemoryBarrier barriers[2] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barriers[0].buffer = m_vertexBuffer->getBuffer();
    barriers[0].size = VK_WHOLE_SIZE;
    
    barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
    barriers[1].buffer = m_indexBuffer->getBuffer();
    barriers[1].size = VK_WHOLE_SIZE;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                        0, 0, nullptr, 2, barriers, 0, nullptr);
    
    m_context->endSingleTimeCommands(cmd);
    
    return true;
}

bool VulkanBlockHighlighter::createShaders() {
    // Load compiled shaders
    auto loadShader = [this](const char* path) -> VkShaderModule {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "[VulkanBlockHighlighter] Failed to open shader: " << path << "\n";
            return VK_NULL_HANDLE;
        }
        
        size_t fileSize = file.tellg();
        std::vector<char> code(fileSize);
        file.seekg(0);
        file.read(code.data(), fileSize);
        file.close();
        
        VkShaderModuleCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        
        VkShaderModule shaderModule;
        if (vkCreateShaderModule(m_context->device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            std::cerr << "[VulkanBlockHighlighter] Failed to create shader module\n";
            return VK_NULL_HANDLE;
        }
        
        return shaderModule;
    };
    
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

    std::string vertPath = exeDir + "/shaders/vulkan/highlight.vert.spv";
    std::string fragPath = exeDir + "/shaders/vulkan/highlight.frag.spv";
    
    m_vertexShader = loadShader(vertPath.c_str());
    m_fragmentShader = loadShader(fragPath.c_str());
    
    return m_vertexShader != VK_NULL_HANDLE && m_fragmentShader != VK_NULL_HANDLE;
}

bool VulkanBlockHighlighter::createPipeline() {
    // Push constants for MVP matrix
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(glm::mat4);
    
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;
    
    if (vkCreatePipelineLayout(m_context->device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[VulkanBlockHighlighter] Failed to create pipeline layout\n";
        return false;
    }
    
    // Shader stages
    VkPipelineShaderStageCreateInfo vertStage = {};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = m_vertexShader;
    vertStage.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragStage = {};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = m_fragmentShader;
    fragStage.pName = "main";
    
    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};
    
    // Vertex input
    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(glm::vec3);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attribute = {};
    attribute.location = 0;
    attribute.binding = 0;
    attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute.offset = 0;
    
    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attribute;
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    
    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;  // wideLines feature not enabled
    
    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;  // Don't write depth for highlight
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    
    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;
    
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    
    VkFormat swapchainFormat = m_context->getSwapchainFormat();
    VkFormat depthFormat = m_context->getDepthFormat();
    
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &swapchainFormat;
    renderingInfo.depthAttachmentFormat = depthFormat;
    
    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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
    
    if (vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        std::cerr << "[VulkanBlockHighlighter] Failed to create pipeline\n";
        return false;
    }
    
    return true;
}

void VulkanBlockHighlighter::render(VkCommandBuffer cmd, const Vec3& blockPos, const glm::mat4& islandTransform, 
                                   const glm::mat4& viewProjection) {
    // Create model matrix (island transform * block offset)
    glm::mat4 blockOffset = glm::translate(glm::mat4(1.0f), 
        glm::vec3(blockPos.x + 0.5f, blockPos.y + 0.5f, blockPos.z + 0.5f));
    glm::mat4 mvp = viewProjection * islandTransform * blockOffset;
    
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &mvp);
    
    VkBuffer vertexBuffers[] = {m_vertexBuffer->getBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT16);
    
    vkCmdDrawIndexed(cmd, 24, 1, 0, 0, 0);
}
