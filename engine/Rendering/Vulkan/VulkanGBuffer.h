#pragma once

#include "VulkanImage.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>

/**
 * VulkanGBuffer - Deferred rendering G-buffer for Vulkan
 * 
 * Implements the same layout as OpenGL GBuffer:
 * - Attachment 0 (RGB16F): Albedo/Base Color
 * - Attachment 1 (RGB16F): World-Space Normal
 * - Attachment 2 (RGB32F): World Position (high precision)
 * - Attachment 3 (RGBA8):  Block Metadata (type, face, unused)
 * - Depth (D24_UNORM_S8_UINT): Scene depth buffer
 */
class VulkanGBuffer {
public:
    VulkanGBuffer() = default;
    ~VulkanGBuffer() { destroy(); }

    // No copy
    VulkanGBuffer(const VulkanGBuffer&) = delete;
    VulkanGBuffer& operator=(const VulkanGBuffer&) = delete;

    /**
     * Initialize G-buffer with specified dimensions
     * @param device Vulkan device
     * @param allocator VMA allocator
     * @param width Render target width
     * @param height Render target height
     * @return True if successful
     */
    bool initialize(VkDevice device, VmaAllocator allocator, uint32_t width, uint32_t height);

    /**
     * Resize G-buffer (recreates all attachments)
     */
    bool resize(uint32_t width, uint32_t height);

    /**
     * Get render pass for geometry pass (writes to G-buffer)
     */
    VkRenderPass getRenderPass() const { return m_renderPass; }

    /**
     * Get framebuffer for geometry pass
     */
    VkFramebuffer getFramebuffer() const { return m_framebuffer; }

    /**
     * Transition all G-buffer images to shader read layout
     * (Call after geometry pass, before lighting pass)
     */
    void transitionToShaderRead(VkCommandBuffer commandBuffer);

    /**
     * Transition all G-buffer images back to attachment layout
     * (Call before next geometry pass)
     */
    void transitionToAttachment(VkCommandBuffer commandBuffer);

    void destroy();

    // Accessors for binding in lighting pass
    VkImageView getAlbedoView() const { return m_albedo.getView(); }
    VkImageView getNormalView() const { return m_normal.getView(); }
    VkImageView getPositionView() const { return m_position.getView(); }
    VkImageView getMetadataView() const { return m_metadata.getView(); }
    VkImageView getDepthView() const { return m_depth.getView(); }

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    bool createRenderPass();
    bool createFramebuffer();

    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // G-buffer attachments
    VulkanImage m_albedo;    // RGB16F - base color
    VulkanImage m_normal;    // RGB16F - world-space normal
    VulkanImage m_position;  // RGB32F - world position
    VulkanImage m_metadata;  // RGBA8 - block metadata
    VulkanImage m_depth;     // D24S8 - depth/stencil

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
};
