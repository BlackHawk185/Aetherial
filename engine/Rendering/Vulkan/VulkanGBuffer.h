#pragma once

#include "VulkanImage.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>

/**
 * VulkanGBuffer - Deferred rendering G-buffer for Vulkan
 * 
 * Color attachments only - depth managed externally:
 * - Attachment 0 (RGB16F): Albedo/Base Color
 * - Attachment 1 (RGB16F): World-Space Normal
 * - Attachment 2 (RGB32F): World Position (high precision)
 * - Attachment 3 (RGBA8):  Block Metadata (type, face, unused)
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
     * Begin geometry pass (dynamic rendering)
     * @param commandBuffer Command buffer to record into
     * @param depthImage External depth image to use
     * @param depthView External depth image view
     * @param depthLayout Current layout of depth image (UNDEFINED on frame 0, ATTACHMENT on subsequent frames)
     */
    void beginGeometryPass(VkCommandBuffer commandBuffer, VkImage depthImage, VkImageView depthView, VkImageLayout depthLayout);

    /**
     * End geometry pass
     */
    void endGeometryPass(VkCommandBuffer commandBuffer);

    void destroy();

    // Accessors for binding in lighting pass
    VkImageView getAlbedoView() const { return m_albedo.getView(); }
    VkImageView getNormalView() const { return m_normal.getView(); }
    VkImageView getPositionView() const { return m_position.getView(); }
    VkImageView getMetadataView() const { return m_metadata.getView(); }

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // G-buffer attachments (color only)
    VulkanImage m_albedo;    // RGB16F - base color
    VulkanImage m_normal;    // RGB16F - world-space normal
    VulkanImage m_position;  // RGB32F - world position
    VulkanImage m_metadata;  // RGBA8 - block metadata
};
