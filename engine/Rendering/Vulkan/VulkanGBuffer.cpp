#include "VulkanGBuffer.h"
#include <iostream>

bool VulkanGBuffer::initialize(VkDevice device, VmaAllocator allocator, uint32_t width, uint32_t height) {
    destroy();

    m_device = device;
    m_allocator = allocator;
    m_width = width;
    m_height = height;

    // Create albedo texture (RGB16F)
    if (!m_albedo.create(allocator, width, height,
                         VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT)) {
        std::cerr << "❌ Failed to create albedo texture" << std::endl;
        return false;
    }

    // Create normal texture (RGB16F)
    if (!m_normal.create(allocator, width, height,
                         VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT)) {
        std::cerr << "❌ Failed to create normal texture" << std::endl;
        return false;
    }

    // Create position texture (RGB32F)
    if (!m_position.create(allocator, width, height,
                           VK_FORMAT_R32G32B32A32_SFLOAT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)) {
        std::cerr << "❌ Failed to create position texture" << std::endl;
        return false;
    }

    // Create metadata texture (RGBA8)
    if (!m_metadata.create(allocator, width, height,
                           VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)) {
        std::cerr << "❌ Failed to create metadata texture" << std::endl;
        return false;
    }

    std::cout << "✅ Vulkan G-Buffer initialized: " << m_width << "x" << m_height
              << " (4 color attachments, external depth)" << std::endl;

    return true;
}

bool VulkanGBuffer::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) {
        return true;
    }

    return initialize(m_device, m_allocator, width, height);
}

void VulkanGBuffer::beginGeometryPass(VkCommandBuffer commandBuffer, VkImage depthImage, VkImageView depthView, VkImageLayout depthLayout) {
    // Track what layout we're being told depth is in vs what tracker thinks
    auto& tracker = VulkanLayoutTracker::getInstance();
    VkImageLayout trackerLayout = tracker.getCurrentLayout(depthImage);
    
    if (tracker.m_verbose) {
        printf("[LayoutTracker] VulkanGBuffer::beginGeometryPass: passed depthLayout=%s, tracker says %s\n",
               tracker.getLayoutName(depthLayout),
               tracker.getLayoutName(trackerLayout));
    }
    
    // Track render pass expectations
    tracker.recordRenderPassBegin(
        depthImage,
        depthLayout,
        "G-buffer pass");
    
    // Transition all G-buffer images from UNDEFINED to COLOR_ATTACHMENT_OPTIMAL
    m_albedo.transitionLayout(commandBuffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    m_normal.transitionLayout(commandBuffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    m_position.transitionLayout(commandBuffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    m_metadata.transitionLayout(commandBuffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    
    VkRenderingAttachmentInfo colorAttachments[4] = {};
    
    // Albedo
    colorAttachments[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[0].imageView = m_albedo.getView();
    colorAttachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    
    // Normal
    colorAttachments[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[1].imageView = m_normal.getView();
    colorAttachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    
    // Position
    colorAttachments[2].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[2].imageView = m_position.getView();
    colorAttachments[2].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[2].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    
    // Metadata
    colorAttachments[3].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[3].imageView = m_metadata.getView();
    colorAttachments[3].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[3].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depthView;
    depthAttachment.imageLayout = depthLayout;  // Use actual current layout (legal with LOAD_OP_CLEAR)
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, {m_width, m_height}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 4;
    renderingInfo.pColorAttachments = colorAttachments;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(commandBuffer, &renderingInfo);
}

void VulkanGBuffer::endGeometryPass(VkCommandBuffer commandBuffer) {
    vkCmdEndRendering(commandBuffer);
    
    // Transition all attachments to SHADER_READ_ONLY for lighting pass
    m_albedo.transitionLayout(commandBuffer,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    m_normal.transitionLayout(commandBuffer,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    m_position.transitionLayout(commandBuffer,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    m_metadata.transitionLayout(commandBuffer,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    
    // Depth transition handled externally
}

void VulkanGBuffer::destroy() {
    m_albedo.destroy();
    m_normal.destroy();
    m_position.destroy();
    m_metadata.destroy();

    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_width = 0;
    m_height = 0;
}
