#include "VulkanTriangleRenderer.h"
#include <iostream>
#include <cstring>
#include <cstdlib>

// Simple vertex shader (GLSL 450)
const char* VERTEX_SHADER_SOURCE = R"(
#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

layout(push_constant) uniform PushConstants {
    float time;
} pc;

void main() {
    float angle = pc.time;
    mat2 rotation = mat2(
        cos(angle), -sin(angle),
        sin(angle), cos(angle)
    );
    vec2 rotatedPos = rotation * inPosition;
    gl_Position = vec4(rotatedPos, 0.0, 1.0);
    fragColor = inColor;
}
)";

const char* FRAGMENT_SHADER_SOURCE = R"(
#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}
)";

#include <fstream>
#include <filesystem>
#include <sstream>

std::vector<uint32_t> compileGLSL(const char* source, const char* profile) {
    // Write shader to temp file with proper extension
    std::string extension = std::string(profile);
    std::string tempPath = std::filesystem::temp_directory_path().string() + "/temp_shader." + extension;
    std::string spirvPath = std::filesystem::temp_directory_path().string() + "/temp_shader.spv";
    
    std::ofstream outFile(tempPath);
    if (!outFile) {
        std::cerr << "[Vulkan] Failed to write temp shader file\n";
        return {};
    }
    outFile << source;
    outFile.close();
    
    // Use glslangValidator from Vulkan SDK (should be in PATH)
    // -S specifies the shader stage (vert, frag, etc.)
    std::string cmd = "glslangValidator -V -S " + extension + " --target-env vulkan1.3 -o \"" + spirvPath + "\" \"" + tempPath + "\" 2>&1";
    
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << "[Vulkan] Failed to run glslangValidator\n";
        return {};
    }
    
    std::stringstream output;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output << buffer;
    }
    int result = _pclose(pipe);
    
    if (result != 0) {
        std::cerr << "[Vulkan] Shader compilation failed:\n" << output.str() << "\n";
        return {};
    }
    
    // Read SPIR-V
    std::ifstream spirvFile(spirvPath, std::ios::binary | std::ios::ate);
    if (!spirvFile) {
        std::cerr << "[Vulkan] Failed to read compiled SPIR-V\n";
        return {};
    }
    
    size_t fileSize = spirvFile.tellg();
    std::vector<uint32_t> spirv(fileSize / sizeof(uint32_t));
    spirvFile.seekg(0);
    spirvFile.read(reinterpret_cast<char*>(spirv.data()), fileSize);
    spirvFile.close();
    
    // Cleanup temp files
    std::filesystem::remove(tempPath);
    std::filesystem::remove(spirvPath);
    
    return spirv;
}

VulkanTriangleRenderer::~VulkanTriangleRenderer() {
    cleanup();
}

bool VulkanTriangleRenderer::init(VulkanContext* context) {
    m_context = context;
    
    if (!createVertexBuffer()) return false;
    if (!createGraphicsPipeline()) return false;
    
    std::cout << "[Vulkan] Triangle renderer initialized\n";
    return true;
}

bool VulkanTriangleRenderer::createVertexBuffer() {
    Vertex vertices[] = {
        {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
        {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
        {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}
    };
    
    VkDeviceSize bufferSize = sizeof(vertices);
    
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_context->getPhysicalDevice(), &memProperties);
    
    // Try integrated GPU path first: Create buffer and check if HOST_VISIBLE+DEVICE_LOCAL memory exists
    VkBufferCreateInfo testBufferInfo{};
    testBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    testBufferInfo.size = bufferSize;
    testBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    testBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkBuffer testBuffer;
    if (vkCreateBuffer(m_context->getDevice(), &testBufferInfo, nullptr, &testBuffer) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create test buffer\n";
        return false;
    }
    
    VkMemoryRequirements testMemReq;
    vkGetBufferMemoryRequirements(m_context->getDevice(), testBuffer, &testMemReq);
    
    // Check if any COMPATIBLE memory type is HOST_VISIBLE + DEVICE_LOCAL
    uint32_t integratedMemoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if (testMemReq.memoryTypeBits & (1 << i)) {
            auto flags = memProperties.memoryTypes[i].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                integratedMemoryType = i;
                break;
            }
        }
    }
    
    // INTEGRATED GPU PATH: Direct upload to shared memory
    if (integratedMemoryType != UINT32_MAX) {
        std::cout << "[Vulkan] Detected integrated/shared GPU memory (type " << integratedMemoryType << ") - using direct upload\n";
        
        m_vertexBuffer = testBuffer;
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = testMemReq.size;
        allocInfo.memoryTypeIndex = integratedMemoryType;
        
        if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &m_vertexBufferMemory) != VK_SUCCESS) {
            std::cerr << "[Vulkan] Failed to allocate vertex buffer memory\n";
            vkDestroyBuffer(m_context->getDevice(), m_vertexBuffer, nullptr);
            return false;
        }
        
        vkBindBufferMemory(m_context->getDevice(), m_vertexBuffer, m_vertexBufferMemory, 0);
        
        void* data;
        vkMapMemory(m_context->getDevice(), m_vertexBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, vertices, bufferSize);
        vkUnmapMemory(m_context->getDevice(), m_vertexBufferMemory);
        
        std::cout << "[Vulkan] ✅ Vertex buffer created (integrated GPU, 3 vertices)\n";
        return true;
    }
    
    // DISCRETE GPU PATH: Must use DEVICE_LOCAL memory
    // NVIDIA discrete GPUs don't allow HOST_VISIBLE for pure vertex buffers
    vkDestroyBuffer(m_context->getDevice(), testBuffer, nullptr);
    std::cout << "[Vulkan] Discrete GPU - using DEVICE_LOCAL memory\n";
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(m_context->getDevice(), &bufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create vertex buffer\n";
        return false;
    }
    
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_context->getDevice(), m_vertexBuffer, &memReq);
    
    std::cout << "[Vulkan] Vertex buffer memory requirements:\n";
    std::cout << "  MemoryTypeBits: 0x" << std::hex << memReq.memoryTypeBits << std::dec << "\n";
    std::cout << "  Available memory types:\n";
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        std::cout << "    Type " << i << ": ";
        auto flags = memProperties.memoryTypes[i].propertyFlags;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) std::cout << "DEVICE_LOCAL ";
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) std::cout << "HOST_VISIBLE ";
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) std::cout << "HOST_COHERENT ";
        if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) std::cout << "HOST_CACHED ";
        if (memReq.memoryTypeBits & (1 << i)) std::cout << " <-- COMPATIBLE";
        std::cout << "\n";
    }
    
    // Find DEVICE_LOCAL memory (best for GPU rendering)
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryType = i;
            std::cout << "[Vulkan] Selected DEVICE_LOCAL memory type " << i << "\n";
            break;
        }
    }
    
    if (memoryType == UINT32_MAX) {
        // Fallback: any compatible memory
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if (memReq.memoryTypeBits & (1 << i)) {
                memoryType = i;
                std::cout << "[Vulkan] Selected memory type " << i << " (fallback)\n";
                break;
            }
        }
    }
    
    if (memoryType == UINT32_MAX) {
        std::cerr << "[Vulkan] Failed to find ANY compatible memory\n";
        vkDestroyBuffer(m_context->getDevice(), m_vertexBuffer, nullptr);
        return false;
    }
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryType;
    
    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &m_vertexBufferMemory) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to allocate vertex buffer memory\n";
        vkDestroyBuffer(m_context->getDevice(), m_vertexBuffer, nullptr);
        return false;
    }
    
    vkBindBufferMemory(m_context->getDevice(), m_vertexBuffer, m_vertexBufferMemory, 0);
    
    // Upload data using vkCmdUpdateBuffer (works for small buffers < 65536 bytes)
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandPool = m_context->getCommandPool();
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_context->getDevice(), &cmdAllocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    vkCmdUpdateBuffer(commandBuffer, m_vertexBuffer, 0, bufferSize, vertices);
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_context->getGraphicsQueue());
    
    vkFreeCommandBuffers(m_context->getDevice(), m_context->getCommandPool(), 1, &commandBuffer);
    
    std::cout << "[Vulkan] ✅ Vertex buffer created (discrete GPU, DEVICE_LOCAL, 3 vertices)\n";
    return true;
}

VkShaderModule VulkanTriangleRenderer::createShaderModule(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();
    
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create shader module\n";
        return VK_NULL_HANDLE;
    }
    
    return shaderModule;
}

bool VulkanTriangleRenderer::createGraphicsPipeline() {
    auto vertShaderCode = compileGLSL(VERTEX_SHADER_SOURCE, "vert");
    auto fragShaderCode = compileGLSL(FRAGMENT_SHADER_SOURCE, "frag");
    
    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        return false;
    }
    
    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);
    
    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
    
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attributeDescriptions[2]{};
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex, pos);
    
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex, color);
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 2;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions;
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_context->getSwapchainExtent().width;
    viewport.height = (float)m_context->getSwapchainExtent().height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_context->getSwapchainExtent();
    
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(float);
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create pipeline layout\n";
        return false;
    }
    
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_context->getRenderPass();
    pipelineInfo.subpass = 0;
    
    if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create graphics pipeline\n";
        return false;
    }
    
    vkDestroyShaderModule(m_context->getDevice(), fragShaderModule, nullptr);
    vkDestroyShaderModule(m_context->getDevice(), vertShaderModule, nullptr);
    
    return true;
}

void VulkanTriangleRenderer::render(VkCommandBuffer commandBuffer, float time) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
    
    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &time);
    
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void VulkanTriangleRenderer::cleanup() {
    if (!m_context || m_context->getDevice() == VK_NULL_HANDLE) return;
    
    if (m_graphicsPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->getDevice(), m_graphicsPipeline, nullptr);
        m_graphicsPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->getDevice(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_context->getDevice(), m_vertexBuffer, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), m_vertexBufferMemory, nullptr);
        m_vertexBufferMemory = VK_NULL_HANDLE;
    }
}
