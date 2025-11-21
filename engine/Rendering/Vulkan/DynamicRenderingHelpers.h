#pragma once

#include <vulkan/vulkan.h>

/**
 * DynamicRenderingHelpers - Utility functions for Vulkan 1.3 dynamic rendering
 * Eliminates the need for VkRenderPass and VkFramebuffer
 */
namespace DynamicRendering {

/**
 * Begin dynamic rendering for depth-only pass (shadow maps)
 */
inline void beginDepthOnlyRendering(
    VkCommandBuffer cmd,
    VkImageView depthView,
    VkExtent2D extent,
    VkClearValue clearValue = {.depthStencil = {1.0f, 0}}
) {
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depthView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue = clearValue;

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);
}

/**
 * Begin dynamic rendering for G-buffer (4 color + depth)
 */
inline void beginGBufferRendering(
    VkCommandBuffer cmd,
    VkImageView albedoView,
    VkImageView normalView,
    VkImageView positionView,
    VkImageView metadataView,
    VkImageView depthView,
    VkExtent2D extent
) {
    VkRenderingAttachmentInfo colorAttachments[4] = {};
    
    // Albedo
    colorAttachments[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[0].imageView = albedoView;
    colorAttachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    
    // Normal
    colorAttachments[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[1].imageView = normalView;
    colorAttachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    
    // Position
    colorAttachments[2].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[2].imageView = positionView;
    colorAttachments[2].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[2].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    
    // Metadata
    colorAttachments[3].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[3].imageView = metadataView;
    colorAttachments[3].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[3].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depthView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 4;
    renderingInfo.pColorAttachments = colorAttachments;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);
}

/**
 * Begin dynamic rendering for HDR lighting pass (color + depth read-only)
 */
inline void beginHDRLightingRendering(
    VkCommandBuffer cmd,
    VkImageView hdrView,
    VkImageView depthView,
    VkExtent2D extent
) {
    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = hdrView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depthView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // Keep G-buffer depth
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);
}

/**
 * Begin dynamic rendering for swapchain (final composite)
 */
inline void beginSwapchainRendering(
    VkCommandBuffer cmd,
    VkImageView swapchainView,
    VkImageView depthView,
    VkExtent2D extent
) {
    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = swapchainView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depthView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);
}

/**
 * End dynamic rendering (replaces vkCmdEndRenderPass)
 */
inline void endRendering(VkCommandBuffer cmd) {
    vkCmdEndRendering(cmd);
}

/**
 * Create pipeline rendering info for dynamic rendering pipelines
 * Use this in VkGraphicsPipelineCreateInfo.pNext
 */
inline VkPipelineRenderingCreateInfo createPipelineRenderingInfo(
    uint32_t colorAttachmentCount,
    const VkFormat* pColorAttachmentFormats,
    VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED,
    VkFormat stencilAttachmentFormat = VK_FORMAT_UNDEFINED
) {
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = colorAttachmentCount;
    renderingInfo.pColorAttachmentFormats = pColorAttachmentFormats;
    renderingInfo.depthAttachmentFormat = depthAttachmentFormat;
    renderingInfo.stencilAttachmentFormat = stencilAttachmentFormat;
    return renderingInfo;
}

} // namespace DynamicRendering
