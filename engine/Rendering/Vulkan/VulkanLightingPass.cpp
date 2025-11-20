#include "VulkanLightingPass.h"
#include <iostream>
#include <fstream>
#include <filesystem>

bool VulkanLightingPass::initialize(VkDevice device, VmaAllocator allocator, VkPipelineCache pipelineCache,
                                    VkDescriptorSetLayout gBufferDescriptorLayout,
                                    VkFormat outputFormat,
                                    VkRenderPass externalRenderPass)
{
    destroy();

    m_device = device;
    m_allocator = allocator;
    m_pipelineCache = pipelineCache;
    m_gBufferLayout = gBufferDescriptorLayout;
    m_outputFormat = outputFormat;

    // Create cascade uniform buffer
    if (!m_cascadeUniformBuffer.create(m_allocator, sizeof(CascadeUniforms), 
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       VMA_MEMORY_USAGE_CPU_TO_GPU)) {
        std::cerr << "❌ Failed to create cascade uniform buffer" << std::endl;
        return false;
    }

    if (!createDescriptorLayouts()) return false;
    
    // Use external render pass if provided, otherwise create own
    if (externalRenderPass != VK_NULL_HANDLE) {
        m_renderPass = externalRenderPass;
        m_ownsRenderPass = false;
    } else {
        if (!createRenderPass()) return false;
        m_ownsRenderPass = true;
    }
    
    if (!createPipeline()) return false;

    std::cout << "✅ VulkanLightingPass initialized (cascade light maps enabled)" << std::endl;
    return true;
}

VkShaderModule VulkanLightingPass::loadShaderModule(const std::string& filepath) {
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
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create shader module: " << filepath << std::endl;
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

bool VulkanLightingPass::createDescriptorLayouts() {
    // Set 1: Shadow map + cloud noise + cascade uniform + SSR
    VkDescriptorSetLayoutBinding bindings[4] = {};
    
    // Binding 0: Shadow map array (sampler2DArrayShadow)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    // Binding 1: Cloud noise (sampler3D)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    // Binding 2: Cascade uniform buffer
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    // Binding 3: SSR reflections (sampler2D)
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_lightingLayout) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting descriptor set layout" << std::endl;
        return false;
    }

    // Create descriptor pool
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 3;  // Shadow map + cloud noise + SSR
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1;  // Cascade uniform

    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting descriptor pool" << std::endl;
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_lightingLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_lightingDescriptorSet) != VK_SUCCESS) {
        std::cerr << "❌ Failed to allocate lighting descriptor set" << std::endl;
        return false;
    }

    return true;
}

bool VulkanLightingPass::createRenderPass() {
    // Single color attachment (HDR output)
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = m_outputFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo renderPassInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting render pass" << std::endl;
        return false;
    }

    return true;
}

bool VulkanLightingPass::createPipeline() {
    // Load shaders
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

    m_vertShader = loadShaderModule(exeDir + "/shaders/vulkan/lighting_pass.vert.spv");
    m_fragShader = loadShaderModule(exeDir + "/shaders/vulkan/lighting_pass.frag.spv");
    
    if (!m_vertShader || !m_fragShader) {
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = m_vertShader;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = m_fragShader;
    shaderStages[1].pName = "main";

    // Push constants
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(PushConstants);

    // Pipeline layout (Set 0 = G-buffer, Set 1 = lighting)
    VkDescriptorSetLayout setLayouts[2] = {m_gBufferLayout, m_lightingLayout};
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting pipeline layout" << std::endl;
        return false;
    }

    // Vertex input (none - fullscreen triangle from vertex ID)
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
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

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

    if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        std::cerr << "❌ Failed to create lighting pipeline" << std::endl;
        return false;
    }

    std::cout << "✅ VulkanLightingPass: Pipeline created successfully (handle: " << m_pipeline << ")" << std::endl;
    return true;
}



bool VulkanLightingPass::updateCloudNoiseDescriptor(VkImageView cloudNoiseTexture) {
    if (m_cloudNoiseSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_cloudNoiseSampler) != VK_SUCCESS) {
            std::cerr << "❌ Failed to create cloud noise sampler" << std::endl;
            return false;
        }
    }

    VkDescriptorImageInfo cloudNoiseInfo = {};
    cloudNoiseInfo.sampler = m_cloudNoiseSampler;
    cloudNoiseInfo.imageView = cloudNoiseTexture;
    cloudNoiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_lightingDescriptorSet;
    write.dstBinding = 1;  // Cloud noise is now binding 1
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &cloudNoiseInfo;

    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    return true;
}

bool VulkanLightingPass::updateDescriptorSet(const VulkanShadowMap& shadowMap, VkImageView cloudNoiseTexture, VkImageView ssrTexture) {
    // Create shadow sampler (for sampler2DArrayShadow)
    if (m_shadowSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.compareEnable = VK_TRUE;  // Enable depth comparison
        samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS) {
            std::cerr << "❌ Failed to create shadow sampler" << std::endl;
            return false;
        }
    }

    // Create cloud noise sampler
    if (m_cloudNoiseSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_cloudNoiseSampler) != VK_SUCCESS) {
            std::cerr << "❌ Failed to create cloud noise sampler" << std::endl;
            return false;
        }
    }

    // Create SSR sampler
    if (m_ssrSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_ssrSampler) != VK_SUCCESS) {
            std::cerr << "❌ Failed to create SSR sampler" << std::endl;
            return false;
        }
    }

    VkWriteDescriptorSet writes[4] = {};

    // Binding 0: Shadow map array
    VkDescriptorImageInfo shadowInfo = {};
    shadowInfo.sampler = m_shadowSampler;
    shadowInfo.imageView = shadowMap.getView();
    shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_lightingDescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &shadowInfo;

    // Binding 1: Cloud noise
    VkDescriptorImageInfo cloudInfo = {};
    cloudInfo.sampler = m_cloudNoiseSampler;
    cloudInfo.imageView = cloudNoiseTexture;
    cloudInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_lightingDescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &cloudInfo;

    // Binding 2: Cascade uniform buffer
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = m_cascadeUniformBuffer.getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(CascadeUniforms);

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_lightingDescriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo = &bufferInfo;

    // Binding 3: SSR reflections
    VkDescriptorImageInfo ssrInfo = {};
    ssrInfo.sampler = m_ssrSampler;
    ssrInfo.imageView = ssrTexture;
    ssrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_lightingDescriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &ssrInfo;

    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    return true;
}

void VulkanLightingPass::updateCascadeUniforms(const CascadeUniforms& cascades) {
    m_cascadeUniformBuffer.upload(&cascades, sizeof(CascadeUniforms));
}

bool VulkanLightingPass::bindTextures(const VulkanShadowMap& shadowMap, VkImageView cloudNoiseTexture, VkImageView ssrTexture) {
    return updateDescriptorSet(shadowMap, cloudNoiseTexture, ssrTexture);
}

void VulkanLightingPass::render(VkCommandBuffer commandBuffer,
                                VkDescriptorSet gBufferDescriptorSet,
                                VkImageView cloudNoiseTexture,
                                const PushConstants& params)
{
    // Update cloud noise descriptor
    updateCloudNoiseDescriptor(cloudNoiseTexture);

    // Bind pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Bind descriptor sets (Set 0 = G-buffer, Set 1 = lighting)
    VkDescriptorSet sets[2] = {gBufferDescriptorSet, m_lightingDescriptorSet};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                           0, 2, sets, 0, nullptr);

    // Push constants
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, sizeof(PushConstants), &params);

    // Draw fullscreen triangle
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void VulkanLightingPass::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    // Wait for GPU before destroying resources (necessary during cleanup)
    vkDeviceWaitIdle(m_device);

    if (m_shadowSampler) {
        vkDestroySampler(m_device, m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }

    if (m_cloudNoiseSampler) {
        vkDestroySampler(m_device, m_cloudNoiseSampler, nullptr);
        m_cloudNoiseSampler = VK_NULL_HANDLE;
    }

    if (m_ssrSampler) {
        vkDestroySampler(m_device, m_ssrSampler, nullptr);
        m_ssrSampler = VK_NULL_HANDLE;
    }

    if (m_descriptorPool) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    
    m_cascadeUniformBuffer.destroy();

    if (m_pipeline) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }

    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    if (m_lightingLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_lightingLayout, nullptr);
        m_lightingLayout = VK_NULL_HANDLE;
    }

    if (m_renderPass && m_ownsRenderPass) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    if (m_fragShader) {
        vkDestroyShaderModule(m_device, m_fragShader, nullptr);
        m_fragShader = VK_NULL_HANDLE;
    }

    if (m_vertShader) {
        vkDestroyShaderModule(m_device, m_vertShader, nullptr);
        m_vertShader = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
}
