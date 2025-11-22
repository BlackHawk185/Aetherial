#include "VulkanDeferred.h"
#include "VulkanContext.h"
#include "ShaderPaths.h"
#include <GLFW/glfw3.h>
#include <iostream>
#include <fstream>

bool VulkanDeferred::initialize(VkDevice device, VmaAllocator allocator, VkPipelineCache pipelineCache,
                                 VkFormat swapchainFormat,
                                 uint32_t width, uint32_t height, VkQueue graphicsQueue, VkCommandPool commandPool,
                                 VulkanContext* context)
{
    destroy();

    m_device = device;
    m_allocator = allocator;
    m_pipelineCache = pipelineCache;
    m_swapchainFormat = swapchainFormat;
    m_width = width;
    m_height = height;
    m_context = context;

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

    // Create HDR buffer (RGB16F for intermediate rendering with SSR)
    if (!m_hdrBuffer.create(allocator, width, height,
                            VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
        std::cerr << "❌ Failed to create HDR buffer" << std::endl;
        return false;
    }
    
    // Create lighting pipeline (inline, no separate class)
    if (!createLightingPipeline()) {
        std::cerr << "❌ Failed to create lighting pipeline" << std::endl;
        return false;
    }

    // Initialize SSPR
    if (!m_sspr.initialize(device, allocator, pipelineCache, width, height, graphicsQueue, commandPool)) {
        std::cerr << "❌ Failed to initialize SSPR" << std::endl;
        return false;
    }
    
    // Create composite pipeline
    if (!createCompositePipeline()) {
        std::cerr << "❌ Failed to create composite pipeline" << std::endl;
        return false;
    }

    std::cout << "✅ VulkanDeferred initialized: " << m_width << "x" << m_height << " (HDR + SSR)" << std::endl;
    return true;
}

bool VulkanDeferred::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) {
        return true;
    }

    // Wait for GPU before recreating G-buffer (necessary during resize)
    vkDeviceWaitIdle(m_device);

    // NOTE: No framebuffers to cleanup - using swapchain framebuffers

    // Resize G-buffer
    if (!m_gbuffer.resize(width, height)) {
        return false;
    }
    
    // Resize HDR buffer (no framebuffer needed with dynamic rendering)
    m_hdrBuffer.destroy();
    if (!m_hdrBuffer.create(m_allocator, width, height,
                            VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
        return false;
    }

    // Resize SSPR
    if (!m_sspr.resize(width, height)) {
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

    // Dynamic rendering: specify color formats instead of render pass
    VkFormat colorFormats[4] = {
        VK_FORMAT_R16G16B16A16_SFLOAT,  // Albedo
        VK_FORMAT_R16G16B16A16_SFLOAT,  // Normal
        VK_FORMAT_R32G32B32A32_SFLOAT,  // Position
        VK_FORMAT_R8G8B8A8_UNORM        // Metadata
    };
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = 4;
    renderingInfo.pColorAttachmentFormats = colorFormats;
    renderingInfo.depthAttachmentFormat = m_context->getDepthFormat();  // D32_SFLOAT

    VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &renderingInfo;
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

    result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_geometryPipeline);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create geometry pipeline" << std::endl;
        return false;
    }

    return true;
}

bool VulkanDeferred::createLightingPipeline() {
    // Load shaders
    m_lightingVertShader = loadShaderModule(ShaderPaths::lighting_pass_vert_spv);
    m_lightingFragShader = loadShaderModule(ShaderPaths::lighting_pass_frag_spv);
    
    if (!m_lightingVertShader || !m_lightingFragShader) {
        std::cerr << "❌ Failed to load lighting shaders" << std::endl;
        return false;
    }

    // Create cascade uniform buffer
    if (!m_cascadeUniformBuffer.create(m_allocator, sizeof(CascadeUniforms), 
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       VMA_MEMORY_USAGE_CPU_TO_GPU)) {
        std::cerr << "❌ Failed to create cascade uniform buffer" << std::endl;
        return false;
    }

    // Create shadow/noise descriptor layout (set 1)
    VkDescriptorSetLayoutBinding shadowBindings[4] = {};
    shadowBindings[0].binding = 0;
    shadowBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBindings[0].descriptorCount = 1;
    shadowBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    shadowBindings[1].binding = 1;
    shadowBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBindings[1].descriptorCount = 1;
    shadowBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    shadowBindings[2].binding = 2;
    shadowBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    shadowBindings[2].descriptorCount = 1;
    shadowBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    shadowBindings[3].binding = 3;
    shadowBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBindings[3].descriptorCount = 1;
    shadowBindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo shadowLayoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    shadowLayoutInfo.bindingCount = 4;
    shadowLayoutInfo.pBindings = shadowBindings;

    if (vkCreateDescriptorSetLayout(m_device, &shadowLayoutInfo, nullptr, &m_shadowDescriptorLayout) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create shadow descriptor layout" << std::endl;
        return false;
    }

    // Create shadow descriptor pool
    VkDescriptorPoolSize shadowPoolSizes[2] = {};
    shadowPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowPoolSizes[0].descriptorCount = 3;
    shadowPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    shadowPoolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo shadowPoolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    shadowPoolInfo.poolSizeCount = 2;
    shadowPoolInfo.pPoolSizes = shadowPoolSizes;
    shadowPoolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_device, &shadowPoolInfo, nullptr, &m_shadowDescriptorPool) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create shadow descriptor pool" << std::endl;
        return false;
    }

    // Allocate shadow descriptor set
    VkDescriptorSetAllocateInfo shadowAllocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    shadowAllocInfo.descriptorPool = m_shadowDescriptorPool;
    shadowAllocInfo.descriptorSetCount = 1;
    shadowAllocInfo.pSetLayouts = &m_shadowDescriptorLayout;

    if (vkAllocateDescriptorSets(m_device, &shadowAllocInfo, &m_shadowDescriptorSet) != VK_SUCCESS) {
        std::cerr << "❌ Failed to allocate shadow descriptor set" << std::endl;
        return false;
    }

    // Create samplers
    VkSamplerCreateInfo shadowSamplerInfo = {};
    shadowSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    shadowSamplerInfo.magFilter = VK_FILTER_LINEAR;
    shadowSamplerInfo.minFilter = VK_FILTER_LINEAR;
    shadowSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSamplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    shadowSamplerInfo.compareEnable = VK_TRUE;
    shadowSamplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    if (vkCreateSampler(m_device, &shadowSamplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create shadow sampler" << std::endl;
        return false;
    }

    VkSamplerCreateInfo cloudSamplerInfo = {};
    cloudSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    cloudSamplerInfo.magFilter = VK_FILTER_LINEAR;
    cloudSamplerInfo.minFilter = VK_FILTER_LINEAR;
    cloudSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    cloudSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    cloudSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    if (vkCreateSampler(m_device, &cloudSamplerInfo, nullptr, &m_cloudNoiseSampler) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create cloud noise sampler" << std::endl;
        return false;
    }

    VkSamplerCreateInfo ssrSamplerInfo = {};
    ssrSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ssrSamplerInfo.magFilter = VK_FILTER_LINEAR;
    ssrSamplerInfo.minFilter = VK_FILTER_LINEAR;
    ssrSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ssrSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ssrSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(m_device, &ssrSamplerInfo, nullptr, &m_ssrSampler) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create SSR sampler" << std::endl;
        return false;
    }

    // Create pipeline layout (set 0 = G-buffer, set 1 = shadows/noise)
    VkDescriptorSetLayout setLayouts[] = {m_lightingDescriptorLayout, m_shadowDescriptorLayout};
    
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(glm::vec4) * 6;  // sun/moon dir+color, camera pos, cascade params

    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_lightingPipelineLayout) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting pipeline layout" << std::endl;
        return false;
    }

    // Create graphics pipeline (fullscreen quad)
    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = m_lightingVertShader;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = m_lightingFragShader;
    shaderStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = 1;
    VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    renderingInfo.pColorAttachmentFormats = &hdrFormat;
    renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;  // No depth attachment in lighting pass

    VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &renderingInfo;
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
    pipelineInfo.layout = m_lightingPipelineLayout;

    if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_lightingPipeline) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting pipeline" << std::endl;
        return false;
    }

    std::cout << "✅ Lighting pipeline created (inline, cascade light maps enabled)" << std::endl;
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

    // Depth descriptor (binding 4) NOT written at init - depth is UNDEFINED
    // Will be written dynamically before first lighting pass when depth is READ_ONLY

    VkWriteDescriptorSet writes[4] = {};  // Only 4 descriptors (skip depth)
    for (int i = 0; i < 4; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_lightingDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }

    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);

    return true;
}

void VulkanDeferred::beginGeometryPass(VkCommandBuffer commandBuffer, VkImage depthImage, VkImageView depthView, VkImageLayout depthLayout) {
    // Pass external depth buffer to G-buffer (D32_SFLOAT from VulkanContext)
    m_gbuffer.beginGeometryPass(commandBuffer, depthImage, depthView, depthLayout);
    
    // Set viewport and scissor
    VkViewport viewport = {0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void VulkanDeferred::endGeometryPass(VkCommandBuffer commandBuffer) {
    m_gbuffer.endGeometryPass(commandBuffer);
}

void VulkanDeferred::computeSSR(VkCommandBuffer commandBuffer,
                                const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix) {
    // SSPR raymarches lit HDR buffer for planar water reflections
    glm::vec3 cameraPos = glm::vec3(glm::inverse(viewMatrix)[3]);
    float time = static_cast<float>(glfwGetTime());
    m_sspr.compute(commandBuffer,
                   m_gbuffer.getNormalView(),
                   m_gbuffer.getPositionView(),
                   m_context->getDepthImageView(),
                   m_gbuffer.getMetadataView(),
                   m_hdrBuffer.getView(),
                   viewMatrix,
                   projectionMatrix,
                   cameraPos,
                   time);
}

void VulkanDeferred::renderLightingToHDR(VkCommandBuffer commandBuffer,
                                         const LightingParams& params,
                                         const CascadeUniforms& cascades,
                                         VkImageView cloudNoiseTexture)
{
    // Transition HDR buffer to COLOR_ATTACHMENT_OPTIMAL (from UNDEFINED on first frame, SHADER_READ_ONLY on subsequent)
    static bool firstFrame = true;
    VkImageLayout oldLayout = firstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkPipelineStageFlags srcStage = firstFrame ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    firstFrame = false;
    
    m_hdrBuffer.transitionLayout(commandBuffer, oldLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 srcStage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    
    // Update cascade uniforms
    void* data = m_cascadeUniformBuffer.map();
    memcpy(data, &cascades, sizeof(CascadeUniforms));
    m_cascadeUniformBuffer.unmap();

    // Dynamic rendering: HDR color + G-buffer depth (read-only)
    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = m_hdrBuffer.getView();
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    
    // Lighting pass: no depth attachment (reads depth from descriptor, no depth writes)
    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, {m_width, m_height}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = nullptr;  // No depth writes in lighting pass

    vkCmdBeginRendering(commandBuffer, &renderingInfo);
    
    // Bind lighting pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipeline);
    
    // Bind descriptor sets (set 0 = G-buffer, set 1 = shadows/noise)
    VkDescriptorSet sets[] = {m_lightingDescriptorSet, m_shadowDescriptorSet};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipelineLayout,
                            0, 2, sets, 0, nullptr);
    
    // Push constants
    struct {
        glm::vec4 sunDirection;
        glm::vec4 moonDirection;
        glm::vec4 sunColor;
        glm::vec4 moonColor;
        glm::vec4 cameraPos;
        glm::vec4 cascadeParams;
    } pushConstants;
    
    pushConstants.sunDirection = params.sunDirection;
    pushConstants.moonDirection = params.moonDirection;
    pushConstants.sunColor = params.sunColor;
    pushConstants.moonColor = params.moonColor;
    pushConstants.cameraPos = params.cameraPos;
    pushConstants.cascadeParams = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    
    vkCmdPushConstants(commandBuffer, m_lightingPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pushConstants), &pushConstants);
    
    // Set viewport/scissor
    VkViewport viewport = {0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    // Draw fullscreen triangle
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    
    vkCmdEndRendering(commandBuffer);
    
    // Transition HDR to shader read for composition
    m_hdrBuffer.transitionLayout(commandBuffer,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void VulkanDeferred::renderLightingPass(VkCommandBuffer commandBuffer,
                                        VkImageView swapchainImageView,
                                        const LightingParams& params,
                                        const CascadeUniforms& cascades,
                                        VkImageView cloudNoiseTexture)
{
    (void)swapchainImageView;
    (void)cloudNoiseTexture;
    
    // Update cascade uniforms
    void* data = m_cascadeUniformBuffer.map();
    memcpy(data, &cascades, sizeof(CascadeUniforms));
    m_cascadeUniformBuffer.unmap();

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipeline);
    
    VkDescriptorSet sets[] = {m_lightingDescriptorSet, m_shadowDescriptorSet};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipelineLayout,
                            0, 2, sets, 0, nullptr);
    
    struct {
        glm::vec4 sunDirection;
        glm::vec4 moonDirection;
        glm::vec4 sunColor;
        glm::vec4 moonColor;
        glm::vec4 cameraPos;
        glm::vec4 cascadeParams;
    } pushConstants;
    
    pushConstants.sunDirection = params.sunDirection;
    pushConstants.moonDirection = params.moonDirection;
    pushConstants.sunColor = params.sunColor;
    pushConstants.moonColor = params.moonColor;
    pushConstants.cameraPos = params.cameraPos;
    pushConstants.cascadeParams = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    
    vkCmdPushConstants(commandBuffer, m_lightingPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pushConstants), &pushConstants);
    
    VkViewport viewport = {0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

bool VulkanDeferred::bindLightingTextures(VkImageView cloudNoiseTexture) {
    VkDescriptorImageInfo shadowInfo = {};
    shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    shadowInfo.imageView = m_shadowMap.getShadowMapImageView();
    shadowInfo.sampler = m_shadowSampler;

    VkDescriptorImageInfo cloudInfo = {};
    cloudInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cloudInfo.imageView = cloudNoiseTexture;
    cloudInfo.sampler = m_cloudNoiseSampler;

    VkDescriptorBufferInfo cascadeInfo = {};
    cascadeInfo.buffer = m_cascadeUniformBuffer.getBuffer();
    cascadeInfo.offset = 0;
    cascadeInfo.range = sizeof(CascadeUniforms);

    VkDescriptorImageInfo ssrInfo = {};
    ssrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ssrInfo.imageView = m_sspr.getOutputView();
    ssrInfo.sampler = m_ssrSampler;

    VkWriteDescriptorSet writes[4] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_shadowDescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &shadowInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_shadowDescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &cloudInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_shadowDescriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo = &cascadeInfo;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_shadowDescriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &ssrInfo;

    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    return true;
}

void VulkanDeferred::compositeToSwapchain(VkCommandBuffer commandBuffer, const glm::vec3& cameraPos) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipelineLayout, 0, 1, &m_compositeDescriptorSet, 0, nullptr);
    
    // Push camera position for Fresnel calculation
    vkCmdPushConstants(commandBuffer, m_compositePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec3), &cameraPos);
    
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void VulkanDeferred::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    // Wait for GPU before destroying resources (necessary during cleanup)
    vkDeviceWaitIdle(m_device);

    // NOTE: No framebuffers to destroy - using swapchain framebuffers directly
    // NOTE: No geometry pipeline - VulkanQuadRenderer handles geometry rendering

    // Destroy composite resources
    if (m_compositePipeline) vkDestroyPipeline(m_device, m_compositePipeline, nullptr);
    if (m_compositePipelineLayout) vkDestroyPipelineLayout(m_device, m_compositePipelineLayout, nullptr);
    if (m_compositeDescriptorPool) vkDestroyDescriptorPool(m_device, m_compositeDescriptorPool, nullptr);
    if (m_compositeDescriptorLayout) vkDestroyDescriptorSetLayout(m_device, m_compositeDescriptorLayout, nullptr);
    if (m_compositeSampler) vkDestroySampler(m_device, m_compositeSampler, nullptr);

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
    
    // Destroy HDR buffer
    m_hdrBuffer.destroy();
    
    // Destroy lighting resources
    if (m_lightingVertShader) vkDestroyShaderModule(m_device, m_lightingVertShader, nullptr);
    if (m_lightingFragShader) vkDestroyShaderModule(m_device, m_lightingFragShader, nullptr);
    if (m_lightingPipeline) vkDestroyPipeline(m_device, m_lightingPipeline, nullptr);
    if (m_lightingPipelineLayout) vkDestroyPipelineLayout(m_device, m_lightingPipelineLayout, nullptr);
    if (m_shadowDescriptorPool) vkDestroyDescriptorPool(m_device, m_shadowDescriptorPool, nullptr);
    if (m_shadowDescriptorLayout) vkDestroyDescriptorSetLayout(m_device, m_shadowDescriptorLayout, nullptr);
    if (m_shadowSampler) vkDestroySampler(m_device, m_shadowSampler, nullptr);
    if (m_cloudNoiseSampler) vkDestroySampler(m_device, m_cloudNoiseSampler, nullptr);
    if (m_ssrSampler) vkDestroySampler(m_device, m_ssrSampler, nullptr);
    m_cascadeUniformBuffer.destroy();
    
    // Destroy shadow and SSPR
    m_shadowMap.destroy();
    m_sspr.destroy();

    // Destroy G-buffer
    m_gbuffer.destroy();

    m_device = VK_NULL_HANDLE;
}

// HDR render pass/framebuffer removed - using dynamic rendering

bool VulkanDeferred::createCompositePipeline() {
    VkDescriptorSetLayoutBinding bindings[6] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;
    
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_compositeDescriptorLayout) != VK_SUCCESS) {
        return false;
    }
    
    // Push constants for camera position
    VkPushConstantRange pushConstantRange = {};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::vec3);  // cameraPos
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_compositeDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_compositePipelineLayout) != VK_SUCCESS) {
        return false;
    }
    
    VkShaderModule vertShader = loadShaderModule(ShaderPaths::composite_vert_spv);
    VkShaderModule fragShader = loadShaderModule(ShaderPaths::composite_frag_spv);
    if (!vertShader || !fragShader) {
        return false;
    }
    
    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertShader;
    shaderStages[0].pName = "main";
    
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragShader;
    shaderStages[1].pName = "main";
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    
    VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    
    VkPipelineRasterizationStateCreateInfo rasterizer = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    
    VkPipelineMultisampleStateCreateInfo multisampling = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineDepthStencilStateCreateInfo depthStencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_FALSE;  // No depth test for fullscreen composite
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    
    VkPipelineColorBlendStateCreateInfo colorBlending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    
    // Dynamic rendering: specify swapchain format and depth format
    VkFormat swapchainFmt = m_swapchainFormat;
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &swapchainFmt;
    renderingInfo.depthAttachmentFormat = m_context->getDepthFormat();  // D32_SFLOAT
    
    VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &renderingInfo;
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
    pipelineInfo.layout = m_compositePipelineLayout;
    
    VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_compositePipeline);
    
    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);
    
    if (result != VK_SUCCESS) {
        return false;
    }
    
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_compositeSampler) != VK_SUCCESS) {
        return false;
    }
    
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 6;  // hdr, ssr, metadata, normal, position, depth
    
    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_compositeDescriptorPool) != VK_SUCCESS) {
        return false;
    }
    
    VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_compositeDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_compositeDescriptorLayout;
    
    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_compositeDescriptorSet) != VK_SUCCESS) {
        return false;
    }
    
    VkDescriptorImageInfo imageInfos[6] = {};
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = m_hdrBuffer.getView();
    imageInfos[0].sampler = m_compositeSampler;
    
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = m_sspr.getOutputView();
    imageInfos[1].sampler = m_compositeSampler;
    
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].imageView = m_gbuffer.getMetadataView();
    imageInfos[2].sampler = m_compositeSampler;
    
    imageInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[3].imageView = m_gbuffer.getNormalView();
    imageInfos[3].sampler = m_compositeSampler;
    
    imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[4].imageView = m_gbuffer.getPositionView();
    imageInfos[4].sampler = m_compositeSampler;
    
    imageInfos[5].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    imageInfos[5].imageView = m_context->getDepthImageView();
    imageInfos[5].sampler = m_compositeSampler;
    
    VkWriteDescriptorSet writes[6] = {};
    for (int i = 0; i < 6; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_compositeDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    
    vkUpdateDescriptorSets(m_device, 6, writes, 0, nullptr);
    
    return true;
}
