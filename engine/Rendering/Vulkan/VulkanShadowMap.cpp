#include "VulkanShadowMap.h"
#include <iostream>

bool VulkanShadowMap::initialize(VkDevice device, VmaAllocator allocator, 
                                 uint32_t size, uint32_t numCascades)
{
    destroy();

    m_device = device;
    m_allocator = allocator;
    m_size = size;
    m_numCascades = numCascades;
    m_cascades.resize(m_numCascades);

    if (!createShadowImage()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createSampler()) return false;

    std::cout << "✅ VulkanShadowMap: " << m_numCascades << " cascades @ " 
              << m_size << "x" << m_size << std::endl;
    return true;
}

bool VulkanShadowMap::createShadowImage() {
    // Create depth image array (one layer per cascade)
    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent.width = m_size;
    imageInfo.extent.height = m_size;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = m_numCascades;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (!m_shadowImage.createArray(m_allocator, m_size, m_size, m_numCascades,
                                    VK_FORMAT_D32_SFLOAT,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT,
                                    VMA_MEMORY_USAGE_GPU_ONLY)) {
        std::cerr << "❌ Failed to create shadow depth image array" << std::endl;
        return false;
    }
    
    // NOTE: Shadow map starts in UNDEFINED layout
    // TODO: Transition when we implement actual shadow rendering
    // For now, lighting pass should not sample from uninitialized shadows

    return true;
}

bool VulkanShadowMap::createRenderPass() {
    // Depth-only render pass for shadow map rendering
    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef = {};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkRenderPassCreateInfo renderPassInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    VkResult result = vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create shadow render pass" << std::endl;
        return false;
    }

    return true;
}

bool VulkanShadowMap::createFramebuffers() {
    m_cascadeImageViews.resize(m_numCascades);
    m_framebuffers.resize(m_numCascades);

    for (uint32_t i = 0; i < m_numCascades; ++i) {
        // Create per-cascade image view (single layer of array)
        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_shadowImage.getImage();
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = i;
        viewInfo.subresourceRange.layerCount = 1;

        VkResult result = vkCreateImageView(m_device, &viewInfo, nullptr, &m_cascadeImageViews[i]);
        if (result != VK_SUCCESS) {
            std::cerr << "❌ Failed to create cascade image view " << i << std::endl;
            return false;
        }

        // Create framebuffer for this cascade
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = m_renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &m_cascadeImageViews[i];
        fbInfo.width = m_size;
        fbInfo.height = m_size;
        fbInfo.layers = 1;

        result = vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffers[i]);
        if (result != VK_SUCCESS) {
            std::cerr << "❌ Failed to create cascade framebuffer " << i << std::endl;
            return false;
        }
    }

    return true;
}

bool VulkanShadowMap::createSampler() {
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkResult result = vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowSampler);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create shadow sampler" << std::endl;
        return false;
    }

    return true;
}

bool VulkanShadowMap::resize(uint32_t newSize) {
    if (newSize == m_size) return true;

    vkDeviceWaitIdle(m_device);

    // Cleanup existing resources
    for (auto fb : m_framebuffers) {
        vkDestroyFramebuffer(m_device, fb, nullptr);
    }
    for (auto view : m_cascadeImageViews) {
        vkDestroyImageView(m_device, view, nullptr);
    }
    m_framebuffers.clear();
    m_cascadeImageViews.clear();
    m_shadowImage.destroy();

    m_size = newSize;

    if (!createShadowImage()) return false;
    if (!createFramebuffers()) return false;

    return true;
}

void VulkanShadowMap::beginCascadeRender(VkCommandBuffer commandBuffer, uint32_t cascadeIndex) {
    VkRenderPassBeginInfo beginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    beginInfo.renderPass = m_renderPass;
    beginInfo.framebuffer = m_framebuffers[cascadeIndex];
    beginInfo.renderArea.offset = {0, 0};
    beginInfo.renderArea.extent = {m_size, m_size};

    VkClearValue clearValue = {};
    clearValue.depthStencil.depth = 1.0f;
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.width = static_cast<float>(m_size);
    viewport.height = static_cast<float>(m_size);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.extent = {m_size, m_size};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void VulkanShadowMap::endCascadeRender(VkCommandBuffer commandBuffer, uint32_t cascadeIndex) {
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanShadowMap::transitionForShaderRead(VkCommandBuffer commandBuffer) {
    // Transition entire image array from UNDEFINED to DEPTH_STENCIL_READ_ONLY_OPTIMAL
    // This is correct for sampler2DArrayShadow which expects depth layouts
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;  // No prior access (UNDEFINED)
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_shadowImage.getImage();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = m_numCascades;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,  // No prior stage
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

void VulkanShadowMap::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_shadowSampler) {
        vkDestroySampler(m_device, m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }

    for (auto fb : m_framebuffers) {
        vkDestroyFramebuffer(m_device, fb, nullptr);
    }
    m_framebuffers.clear();

    for (auto view : m_cascadeImageViews) {
        vkDestroyImageView(m_device, view, nullptr);
    }
    m_cascadeImageViews.clear();

    if (m_renderPass) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    m_shadowImage.destroy();

    m_device = VK_NULL_HANDLE;
}
