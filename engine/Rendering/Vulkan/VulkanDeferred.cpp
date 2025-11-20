#include "VulkanDeferred.h"
#include <iostream>
#include <fstream>

bool VulkanDeferred::initialize(VkDevice device, VmaAllocator allocator, VkPipelineCache pipelineCache,
                                 VkFormat swapchainFormat, VkRenderPass swapchainRenderPass,
                                 uint32_t width, uint32_t height, VkQueue graphicsQueue, VkCommandPool commandPool)
{
    destroy();

    m_device = device;
    m_allocator = allocator;
    m_pipelineCache = pipelineCache;
    m_swapchainFormat = swapchainFormat;
    m_width = width;
    m_height = height;

    // Initialize G-buffer
    if (!m_gbuffer.initialize(device, allocator, width, height)) {
        std::cerr << "❌ Failed to initialize G-buffer" << std::endl;
        return false;
    }

    // NOTE: Geometry pipeline is handled by VulkanQuadRenderer, not VulkanDeferred
    // VulkanDeferred only manages G-buffer creation and lighting pass
    
    // Initialize shadow map system (4 cascades: sun near/far, moon near/far)
    if (!m_shadowMap.initialize(device, allocator, 4096, 4)) {
        std::cerr << "❌ Failed to initialize shadow map" << std::endl;
        return false;
    }

    // Create descriptor set layouts
    if (!createDescriptorSetLayouts()) {
        std::cerr << "❌ Failed to create descriptor set layouts" << std::endl;
        return false;
    }

    // Create G-buffer descriptor set for lighting pass
    if (!createDescriptorSets()) {
        std::cerr << "❌ Failed to create descriptor sets" << std::endl;
        return false;
    }

    // Initialize lighting pass (uses swapchain render pass for compatibility)
    if (!m_lightingPass.initialize(device, allocator, pipelineCache, m_lightingDescriptorLayout, swapchainFormat, swapchainRenderPass)) {
        std::cerr << "❌ Failed to initialize lighting pass" << std::endl;
        return false;
    }

    // Initialize SSR
    if (!m_ssr.initialize(device, allocator, pipelineCache, width, height, graphicsQueue, commandPool)) {
        std::cerr << "❌ Failed to initialize SSR" << std::endl;
        return false;
    }

    std::cout << "✅ VulkanDeferred initialized: " << m_width << "x" << m_height << " (with SSR)" << std::endl;
    return true;
}

bool VulkanDeferred::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) {
        return true;
    }

    // Wait for device idle before resizing
    vkDeviceWaitIdle(m_device);

    // NOTE: No framebuffers to cleanup - using swapchain framebuffers

    // Resize G-buffer
    if (!m_gbuffer.resize(width, height)) {
        return false;
    }

    // Resize SSR
    if (!m_ssr.resize(width, height)) {
        return false;
    }

    m_width = width;
    m_height = height;

    // Recreate descriptor set with new G-buffer views
    if (!createDescriptorSets()) {
        return false;
    }

    return true;
}

VkShaderModule VulkanDeferred::loadShaderModule(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "❌ Failed to open shader file: " << filepath << std::endl;
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
        std::cerr << "❌ Failed to create shader module: " << filepath << " (error " << result << ")" << std::endl;
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

bool VulkanDeferred::loadShaders() {
    m_gbufferVertShader = loadShaderModule("shaders/vulkan/gbuffer.vert.spv");
    if (m_gbufferVertShader == VK_NULL_HANDLE) return false;

    m_gbufferFragShader = loadShaderModule("shaders/vulkan/gbuffer.frag.spv");
    if (m_gbufferFragShader == VK_NULL_HANDLE) return false;

    return true;
}

bool VulkanDeferred::createGeometryPipeline() {
    // Descriptor set layout: Set 0 = transforms SSBO only
    // (Texture atlas will be Set 1, handled separately when integrated)
    VkDescriptorSetLayoutBinding binding0 = {};
    binding0.binding = 0;
    binding0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding0.descriptorCount = 1;
    binding0.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding0;

    VkResult result = vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_geometryDescriptorLayout);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create geometry descriptor set layout" << std::endl;
        return false;
    }

    // Push constants (view-projection matrix)
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_geometryDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    result = vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_geometryPipelineLayout);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create geometry pipeline layout" << std::endl;
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = m_gbufferVertShader;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = m_gbufferFragShader;
    shaderStages[1].pName = "main";

    // Vertex input (QuadVertex + QuadFace attributes - same as VulkanQuadRenderer)
    VkVertexInputBindingDescription bindings2[2] = {};
    bindings2[0].binding = 0;
    bindings2[0].stride = sizeof(float) * 8; // QuadVertex: 3 pos + 2 uv + 3 normal
    bindings2[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    bindings2[1].binding = 1;
    bindings2[1].stride = sizeof(float) * 14 + sizeof(uint32_t) * 2; // QuadFace
    bindings2[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[10] = {};
    // QuadVertex attributes
    attributes[0].binding = 0; attributes[0].location = 0; attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT; attributes[0].offset = 0;
    attributes[1].binding = 0; attributes[1].location = 1; attributes[1].format = VK_FORMAT_R32G32_SFLOAT; attributes[1].offset = 12;
    attributes[2].binding = 0; attributes[2].location = 2; attributes[2].format = VK_FORMAT_R32G32B32_SFLOAT; attributes[2].offset = 20;

    // QuadFace attributes
    uint32_t offset = 0;
    attributes[3].binding = 1; attributes[3].location = 3; attributes[3].format = VK_FORMAT_R32G32B32_SFLOAT; attributes[3].offset = offset; offset += 12;
    attributes[4].binding = 1; attributes[4].location = 4; attributes[4].format = VK_FORMAT_R32G32B32_SFLOAT; attributes[4].offset = offset; offset += 12;
    attributes[5].binding = 1; attributes[5].location = 5; attributes[5].format = VK_FORMAT_R32G32B32A32_SFLOAT; attributes[5].offset = offset; offset += 16;
    attributes[6].binding = 1; attributes[6].location = 6; attributes[6].format = VK_FORMAT_R32G32_SFLOAT; attributes[6].offset = offset; offset += 8;
    attributes[7].binding = 1; attributes[7].location = 7; attributes[7].format = VK_FORMAT_R32G32B32A32_SFLOAT; attributes[7].offset = offset; offset += 16;
    attributes[8].binding = 1; attributes[8].location = 8; attributes[8].format = VK_FORMAT_R32_UINT; attributes[8].offset = offset; offset += 4;
    attributes[9].binding = 1; attributes[9].location = 9; attributes[9].format = VK_FORMAT_R32_UINT; attributes[9].offset = offset;

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInputInfo.vertexBindingDescriptionCount = 2;
    vertexInputInfo.pVertexBindingDescriptions = bindings2;
    vertexInputInfo.vertexAttributeDescriptionCount = 10;
    vertexInputInfo.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};

    VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachments[4] = {};
    for (int i = 0; i < 4; i++) {
        colorBlendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachments[i].blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 4;
    colorBlending.pAttachments = colorBlendAttachments;

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
    pipelineInfo.layout = m_geometryPipelineLayout;
    pipelineInfo.renderPass = m_gbuffer.getRenderPass();
    pipelineInfo.subpass = 0;

    result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_geometryPipeline);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create geometry pipeline" << std::endl;
        return false;
    }

    return true;
}

bool VulkanDeferred::createDescriptorSetLayouts() {
    // Create descriptor set layout for lighting pass (reads G-buffer textures)
    VkDescriptorSetLayoutBinding bindings[5] = {};
    
    // Binding 0: Albedo + AO texture
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 1: Normal texture
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 2: Position texture
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 3: Metadata texture
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 4: Depth texture
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;

    VkResult result = vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_lightingDescriptorLayout);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting descriptor set layout" << std::endl;
        return false;
    }

    // Create sampler for G-buffer texture reads
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
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    result = vkCreateSampler(m_device, &samplerInfo, nullptr, &m_gbufferSampler);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create G-buffer sampler" << std::endl;
        return false;
    }

    std::cout << "✅ Created lighting descriptor set layout and G-buffer sampler" << std::endl;
    return true;
}

bool VulkanDeferred::createDescriptorSets() {
    // Create descriptor pools
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 6; // 1 for texture atlas + 5 for G-buffer

    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 2;

    VkResult result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_geometryDescriptorPool);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create geometry descriptor pool" << std::endl;
        return false;
    }

    result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_lightingDescriptorPool);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting descriptor pool" << std::endl;
        return false;
    }

    // Allocate lighting descriptor set
    VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_lightingDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_lightingDescriptorLayout;

    result = vkAllocateDescriptorSets(m_device, &allocInfo, &m_lightingDescriptorSet);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to allocate lighting descriptor set" << std::endl;
        return false;
    }

    // Update lighting descriptor set with G-buffer textures
    VkDescriptorImageInfo imageInfos[5] = {};
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = m_gbuffer.getAlbedoView();
    imageInfos[0].sampler = m_gbufferSampler;

    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = m_gbuffer.getNormalView();
    imageInfos[1].sampler = m_gbufferSampler;

    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].imageView = m_gbuffer.getPositionView();
    imageInfos[2].sampler = m_gbufferSampler;

    imageInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[3].imageView = m_gbuffer.getMetadataView();
    imageInfos[3].sampler = m_gbufferSampler;

    imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[4].imageView = m_gbuffer.getDepthView();
    imageInfos[4].sampler = m_gbufferSampler;

    VkWriteDescriptorSet writes[5] = {};
    for (int i = 0; i < 5; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_lightingDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }

    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    return true;
}

void VulkanDeferred::beginGeometryPass(VkCommandBuffer commandBuffer) {
    VkRenderPassBeginInfo rpInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpInfo.renderPass = m_gbuffer.getRenderPass();
    rpInfo.framebuffer = m_gbuffer.getFramebuffer();
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {m_width, m_height};

    VkClearValue clearValues[5] = {};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[4].depthStencil = {1.0f, 0};

    rpInfo.clearValueCount = 5;
    rpInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(commandBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // Set viewport and scissor for G-buffer rendering
    VkViewport viewport = {0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    // NOTE: Pipeline binding is done by VulkanQuadRenderer::renderToGBuffer()
}

void VulkanDeferred::endGeometryPass(VkCommandBuffer commandBuffer) {
    // End render pass first
    vkCmdEndRenderPass(commandBuffer);
    
    // CRITICAL: Transition must happen OUTSIDE render pass
    // Transition G-buffer to shader read layout for lighting pass
    m_gbuffer.transitionToShaderRead(commandBuffer);
}

void VulkanDeferred::computeSSR(VkCommandBuffer commandBuffer, VkImageView colorBuffer,
                                const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix) {
    m_ssr.compute(commandBuffer,
                  m_gbuffer.getNormalView(),
                  m_gbuffer.getPositionView(),
                  m_gbuffer.getDepthView(),
                  m_gbuffer.getMetadataView(),
                  colorBuffer,
                  viewMatrix,
                  projectionMatrix);
}

void VulkanDeferred::renderLightingPass(VkCommandBuffer commandBuffer,
                                        VkImageView swapchainImageView,
                                        const LightingParams& params,
                                        const VulkanLightingPass::CascadeUniforms& cascades,
                                        VkImageView cloudNoiseTexture)
{
    // Update cascade uniforms every frame
    m_lightingPass.updateCascadeUniforms(cascades);

    // Convert LightingParams to VulkanLightingPass::PushConstants
    VulkanLightingPass::PushConstants pushConstants;
    pushConstants.sunDirection = params.sunDirection;
    pushConstants.moonDirection = params.moonDirection;
    pushConstants.sunColor = params.sunColor;
    pushConstants.moonColor = params.moonColor;
    pushConstants.cameraPos = params.cameraPos;
    pushConstants.cascadeParams = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); // ditherStrength=1, cloudShadows=enabled

    // Lighting pass now renders inside the swapchain render pass (no separate render pass needed)
    VkViewport viewport = {0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    m_lightingPass.render(commandBuffer, m_lightingDescriptorSet, 
                         cloudNoiseTexture, pushConstants);
}

bool VulkanDeferred::bindLightingTextures(VkImageView cloudNoiseTexture) {
    return m_lightingPass.bindTextures(m_shadowMap, cloudNoiseTexture, m_ssr.getOutputView());
}

void VulkanDeferred::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    // NOTE: No framebuffers to destroy - using swapchain framebuffers directly
    // NOTE: No geometry pipeline - VulkanQuadRenderer handles geometry rendering

    // Destroy descriptor sets/pools/layouts
    if (m_geometryDescriptorPool) vkDestroyDescriptorPool(m_device, m_geometryDescriptorPool, nullptr);
    if (m_lightingDescriptorPool) vkDestroyDescriptorPool(m_device, m_lightingDescriptorPool, nullptr);
    if (m_geometryDescriptorLayout) vkDestroyDescriptorSetLayout(m_device, m_geometryDescriptorLayout, nullptr);
    if (m_lightingDescriptorLayout) vkDestroyDescriptorSetLayout(m_device, m_lightingDescriptorLayout, nullptr);

    // Destroy shaders
    if (m_gbufferVertShader) vkDestroyShaderModule(m_device, m_gbufferVertShader, nullptr);
    if (m_gbufferFragShader) vkDestroyShaderModule(m_device, m_gbufferFragShader, nullptr);

    // Destroy sampler
    if (m_gbufferSampler) vkDestroySampler(m_device, m_gbufferSampler, nullptr);
    
    // Destroy advanced lighting system
    m_lightingPass.destroy();
    m_shadowMap.destroy();

    // Destroy G-buffer
    m_gbuffer.destroy();

    m_device = VK_NULL_HANDLE;
}
