#include "VulkanCloudRenderer.h"
#include "VulkanContext.h"
#include "../Parameters.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>

bool VulkanCloudRenderer::initialize(VulkanContext* context, uint32_t width, uint32_t height)
{
    destroy();

    m_context = context;
    m_device = context->getDevice();
    m_allocator = context->getAllocator();
    m_renderPass = context->getRenderPass();
    m_width = width;
    m_height = height;

    // Load cloud parameters from engine settings
    m_cloudCoverage = EngineParameters::Clouds::CLOUD_COVERAGE;
    m_cloudDensity = EngineParameters::Clouds::CLOUD_DENSITY;
    m_cloudSpeed = EngineParameters::Clouds::CLOUD_SPEED;

    if (!create3DNoiseTexture()) {
        std::cerr << "❌ Failed to create 3D noise texture" << std::endl;
        return false;
    }

    if (!loadShaders()) {
        std::cerr << "❌ Failed to load cloud shaders" << std::endl;
        return false;
    }

    if (!createDescriptorSet()) {
        std::cerr << "❌ Failed to create cloud descriptor set" << std::endl;
        return false;
    }

    if (!createPipeline()) {
        std::cerr << "❌ Failed to create cloud pipeline" << std::endl;
        return false;
    }

    std::cout << "✅ VulkanCloudRenderer initialized: " << m_width << "x" << m_height << std::endl;
    return true;
}

bool VulkanCloudRenderer::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) {
        return true;
    }

    vkDeviceWaitIdle(m_device);

    m_width = width;
    m_height = height;

    return true;
}

VkShaderModule VulkanCloudRenderer::loadShaderModule(const char* filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "❌ Failed to open shader: " << filepath << std::endl;
        return VK_NULL_HANDLE;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = buffer.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    VkShaderModule shaderModule;
    VkResult result = vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create shader module: " << filepath << std::endl;
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

bool VulkanCloudRenderer::loadShaders() {
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

    std::string vertPath = exeDir + "/shaders/vulkan/clouds.vert.spv";
    std::string fragPath = exeDir + "/shaders/vulkan/clouds.frag.spv";
    
    m_vertShader = loadShaderModule(vertPath.c_str());
    if (m_vertShader == VK_NULL_HANDLE) return false;

    m_fragShader = loadShaderModule(fragPath.c_str());
    if (m_fragShader == VK_NULL_HANDLE) return false;

    return true;
}

bool VulkanCloudRenderer::create3DNoiseTexture() {
    const int size = EngineParameters::Clouds::NOISE_TEXTURE_SIZE;
    const char* texturePath = "assets/textures/cloud_noise_3d.bin";

    // Load pre-generated texture
    std::ifstream file(texturePath, std::ios::binary);
    if (!file) {
        std::cerr << "❌ Failed to open cloud noise texture: " << texturePath << std::endl;
        return false;
    }

    // Read header
    char magic[4];
    file.read(magic, 4);
    if (std::string(magic, 4) != "CN3D") {
        std::cerr << "❌ Invalid cloud noise texture format" << std::endl;
        return false;
    }

    uint32_t fileSize;
    file.read(reinterpret_cast<char*>(&fileSize), sizeof(uint32_t));
    if (fileSize != size) {
        std::cerr << "❌ Cloud noise texture size mismatch" << std::endl;
        return false;
    }

    // Read texture data
    std::vector<unsigned char> noiseData(size * size * size);
    file.read(reinterpret_cast<char*>(noiseData.data()), noiseData.size());

    if (!file) {
        std::cerr << "❌ Failed to read cloud noise texture data" << std::endl;
        return false;
    }

    // Create staging buffer
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = noiseData.size();
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuffer;
    VmaAllocation stagingAllocation;
    vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &stagingBuffer, &stagingAllocation, nullptr);

    // Copy data to staging buffer
    void* data;
    vmaMapMemory(m_allocator, stagingAllocation, &data);
    memcpy(data, noiseData.data(), noiseData.size());
    vmaUnmapMemory(m_allocator, stagingAllocation);

    // Create 3D texture
    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent = {(uint32_t)size, (uint32_t)size, (uint32_t)size};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imgAllocInfo = {};
    imgAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkResult result = vmaCreateImage(m_allocator, &imageInfo, &imgAllocInfo, 
                                     &m_noiseTexture, &m_noiseAllocation, nullptr);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create 3D noise texture" << std::endl;
        vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
        return false;
    }

    // Transition to transfer dst
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_noiseTexture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {(uint32_t)size, (uint32_t)size, (uint32_t)size};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_noiseTexture, 
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to shader read
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_context->endSingleTimeCommands(cmd);

    // Cleanup staging buffer
    vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);

    // Create image view
    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_noiseTexture;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    result = vkCreateImageView(m_device, &viewInfo, nullptr, &m_noiseTextureView);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create 3D noise texture view" << std::endl;
        return false;
    }

    std::cout << "✓ Loaded " << size << "^3 cloud noise texture (Vulkan)" << std::endl;
    return true;
}

bool VulkanCloudRenderer::createDescriptorSet() {
    // Create samplers
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    VkResult result = vkCreateSampler(m_device, &samplerInfo, nullptr, &m_depthSampler);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create depth sampler" << std::endl;
        return false;
    }

    // Noise sampler with linear filtering and wrapping
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;

    result = vkCreateSampler(m_device, &samplerInfo, nullptr, &m_noiseSampler);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create noise sampler" << std::endl;
        return false;
    }

    // Descriptor set layout: depth + noise texture
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    result = vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create descriptor set layout" << std::endl;
        return false;
    }

    // Create descriptor pool
    VkDescriptorPoolSize poolSizes[1] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 1;

    result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create descriptor pool" << std::endl;
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorLayout;

    result = vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to allocate descriptor set" << std::endl;
        return false;
    }

    // Update descriptor set with noise texture (depth will be updated per-frame)
    VkDescriptorImageInfo noiseImageInfo = {};
    noiseImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    noiseImageInfo.imageView = m_noiseTextureView;
    noiseImageInfo.sampler = m_noiseSampler;

    VkWriteDescriptorSet writes[1] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 1;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &noiseImageInfo;

    vkUpdateDescriptorSets(m_device, 1, writes, 0, nullptr);

    return true;
}

bool VulkanCloudRenderer::createPipeline() {
    // Push constant range (matrices + params)
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(glm::mat4) * 2 + sizeof(glm::vec4) * 3;  // invProj, invView, cameraPos, sunDir, cloudParams

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    VkResult result = vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create pipeline layout" << std::endl;
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo vertStage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = m_vertShader;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = m_fragShader;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertStage, fragStage};

    // Vertex input (none - fullscreen triangle)
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth/stencil (no depth writes for clouds)
    VkPipelineDepthStencilStateCreateInfo depthStencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Color blending (alpha blend)
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Dynamic states
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;

    result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create cloud pipeline" << std::endl;
        return false;
    }

    return true;
}

void VulkanCloudRenderer::render(VkCommandBuffer cmd, VkImageView depthTexture, 
                                 const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                                 const CloudParams& params)
{
    if (!EngineParameters::Clouds::ENABLE_CLOUDS) {
        std::cout << "Clouds disabled" << std::endl;
        return;
    }

    // No height culling - clouds render everywhere (removed CLOUD_BASE_MIN/MAX_HEIGHT)
    
    // Skip rendering if depth texture is not available (layout transition issue)
    if (depthTexture == VK_NULL_HANDLE) {
        return;
    }
    
    // Update depth texture in descriptor set (dynamic)
    VkDescriptorImageInfo depthImageInfo = {};
    depthImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthImageInfo.imageView = depthTexture;
    depthImageInfo.sampler = m_depthSampler;

    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_descriptorSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &depthImageInfo;

    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

    // Bind pipeline and descriptor set
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 
                           0, 1, &m_descriptorSet, 0, nullptr);

    // Set viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Push constants
    glm::mat4 invProj = glm::inverse(projectionMatrix);
    glm::mat4 invView = glm::inverse(viewMatrix);
    glm::vec4 cameraPos = glm::vec4(params.cameraPosition, 0.0f);
    glm::vec4 sunDir = glm::vec4(params.sunDirection, params.sunIntensity);
    glm::vec4 cloudParams = glm::vec4(params.cloudCoverage, params.cloudDensity, 
                                      params.cloudSpeed, params.timeOfDay);

    uint32_t offset = 0;
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      offset, sizeof(glm::mat4), &invProj);
    offset += sizeof(glm::mat4);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      offset, sizeof(glm::mat4), &invView);
    offset += sizeof(glm::mat4);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      offset, sizeof(glm::vec4), &cameraPos);
    offset += sizeof(glm::vec4);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      offset, sizeof(glm::vec4), &sunDir);
    offset += sizeof(glm::vec4);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      offset, sizeof(glm::vec4), &cloudParams);

    // Draw fullscreen triangle
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void VulkanCloudRenderer::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descriptorPool) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_descriptorLayout) vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
    if (m_depthSampler) vkDestroySampler(m_device, m_depthSampler, nullptr);
    if (m_noiseSampler) vkDestroySampler(m_device, m_noiseSampler, nullptr);
    if (m_noiseTextureView) vkDestroyImageView(m_device, m_noiseTextureView, nullptr);
    if (m_noiseTexture) vmaDestroyImage(m_allocator, m_noiseTexture, m_noiseAllocation);
    if (m_vertShader) vkDestroyShaderModule(m_device, m_vertShader, nullptr);
    if (m_fragShader) vkDestroyShaderModule(m_device, m_fragShader, nullptr);

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorLayout = VK_NULL_HANDLE;
    m_depthSampler = VK_NULL_HANDLE;
    m_noiseSampler = VK_NULL_HANDLE;
    m_noiseTextureView = VK_NULL_HANDLE;
    m_noiseTexture = VK_NULL_HANDLE;
    m_noiseAllocation = VK_NULL_HANDLE;
    m_vertShader = VK_NULL_HANDLE;
    m_fragShader = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
}
