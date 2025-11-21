#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

/**
 * VulkanImage - Wrapper for Vulkan image + view + memory
 * 
 * Simplifies image creation for render targets, depth buffers, and textures.
 * Uses VMA for memory management.
 */
class VulkanImage {
public:
    VulkanImage() = default;
    ~VulkanImage() { destroy(); }

    // No copy
    VulkanImage(const VulkanImage&) = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;

    // Move semantics
    VulkanImage(VulkanImage&& other) noexcept;
    VulkanImage& operator=(VulkanImage&& other) noexcept;

    /**
     * Create 2D image with VMA
     * @param allocator VMA allocator
     * @param width Image width
     * @param height Image height
     * @param format Vulkan format (e.g. VK_FORMAT_R8G8B8A8_SRGB)
     * @param usage Usage flags (e.g. VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
     * @param aspect Aspect mask for view (e.g. VK_IMAGE_ASPECT_COLOR_BIT)
     * @param memUsage VMA memory usage (default: GPU_ONLY)
     * @return True if successful
     */
    bool create(VmaAllocator allocator, uint32_t width, uint32_t height,
                VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                VmaMemoryUsage memUsage = VMA_MEMORY_USAGE_GPU_ONLY);

    /**
     * Create 2D image array (for shadow cascades, texture atlases)
     */
    bool createArray(VmaAllocator allocator, uint32_t width, uint32_t height, uint32_t layers,
                     VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                     VmaMemoryUsage memUsage = VMA_MEMORY_USAGE_GPU_ONLY);

    /**
     * Transition image layout using pipeline barrier
     * @param commandBuffer Command buffer to record barrier
     * @param oldLayout Current layout
     * @param newLayout Desired layout
     * @param srcStage Source pipeline stage
     * @param dstStage Destination pipeline stage
     */
    void transitionLayout(VkCommandBuffer commandBuffer,
                          VkImageLayout oldLayout, VkImageLayout newLayout,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

    void destroy();

    // Getters
    VkImage getImage() const { return m_image; }
    VkImageView getView() const { return m_view; }
    VkFormat getFormat() const { return m_format; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    uint32_t getLayers() const { return m_layers; }
    bool isValid() const { return m_image != VK_NULL_HANDLE; }
    
    // Layout tracking
    VkImageLayout getCurrentLayout() const { return m_currentLayout; }
    void setCurrentLayout(VkImageLayout layout) { m_currentLayout = layout; }
    
    // State-aware transition (uses tracked current layout)
    void transitionTo(VkCommandBuffer cmd, VkImageLayout newLayout, 
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

private:
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;

    VkFormat m_format = VK_FORMAT_UNDEFINED;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_layers = 1;
    VkImageLayout m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags m_aspect = 0;
};
