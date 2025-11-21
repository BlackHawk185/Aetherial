#include "VulkanImage.h"
#include <iostream>

VulkanImage::VulkanImage(VulkanImage&& other) noexcept
    : m_image(other.m_image)
    , m_view(other.m_view)
    , m_allocation(other.m_allocation)
    , m_allocator(other.m_allocator)
    , m_device(other.m_device)
    , m_format(other.m_format)
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_layers(other.m_layers)
    , m_aspect(other.m_aspect)
{
    other.m_image = VK_NULL_HANDLE;
    other.m_view = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_allocator = VK_NULL_HANDLE;
    other.m_device = VK_NULL_HANDLE;
}

VulkanImage& VulkanImage::operator=(VulkanImage&& other) noexcept {
    if (this != &other) {
        destroy();

        m_image = other.m_image;
        m_view = other.m_view;
        m_allocation = other.m_allocation;
        m_allocator = other.m_allocator;
        m_device = other.m_device;
        m_format = other.m_format;
        m_width = other.m_width;
        m_height = other.m_height;
        m_layers = other.m_layers;
        m_aspect = other.m_aspect;

        other.m_image = VK_NULL_HANDLE;
        other.m_view = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_allocator = VK_NULL_HANDLE;
        other.m_device = VK_NULL_HANDLE;
    }
    return *this;
}

bool VulkanImage::create(VmaAllocator allocator, uint32_t width, uint32_t height,
                         VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VmaMemoryUsage memUsage)
{
    return createArray(allocator, width, height, 1, format, usage, aspect, memUsage);
}

bool VulkanImage::createArray(VmaAllocator allocator, uint32_t width, uint32_t height, uint32_t layers,
                               VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                               VmaMemoryUsage memUsage)
{
    destroy();

    m_allocator = allocator;
    m_width = width;
    m_height = height;
    m_layers = layers;
    m_format = format;
    m_aspect = aspect;
    m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Get device from allocator
    VmaAllocatorInfo allocInfo;
    vmaGetAllocatorInfo(allocator, &allocInfo);
    m_device = allocInfo.device;

    // Create image
    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCreateInfo = {};
    allocCreateInfo.usage = memUsage;

    VkResult result = vmaCreateImage(allocator, &imageInfo, &allocCreateInfo,
                                     &m_image, &m_allocation, nullptr);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create Vulkan image: " << result << std::endl;
        return false;
    }

    // Create image view
    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_image;
    viewInfo.viewType = (layers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = layers;

    result = vkCreateImageView(m_device, &viewInfo, nullptr, &m_view);
    if (result != VK_SUCCESS) {
        std::cerr << "❌ Failed to create Vulkan image view: " << result << std::endl;
        vmaDestroyImage(m_allocator, m_image, m_allocation);
        m_image = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void VulkanImage::transitionLayout(VkCommandBuffer commandBuffer,
                                   VkImageLayout oldLayout, VkImageLayout newLayout,
                                   VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = m_aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = m_layers;

    // Determine access masks based on layouts
    VkAccessFlags srcAccessMask = 0;
    VkAccessFlags dstAccessMask = 0;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        srcAccessMask = 0;
    } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    if (newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        dstAccessMask = 0;
    }

    barrier.srcAccessMask = srcAccessMask;
    barrier.dstAccessMask = dstAccessMask;

    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanImage::transitionTo(VkCommandBuffer cmd, VkImageLayout newLayout,
                               VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    if (!m_image) return;
    if (m_currentLayout == newLayout) return; // Already in target layout
    
    transitionLayout(cmd, m_currentLayout, newLayout, srcStage, dstStage);
    m_currentLayout = newLayout;
}

void VulkanImage::destroy() {
    if (m_view != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_view, nullptr);
        m_view = VK_NULL_HANDLE;
    }

    if (m_image != VK_NULL_HANDLE && m_allocator != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
        m_image = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
    }

    m_allocator = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_width = 0;
    m_height = 0;
    m_layers = 1;
    m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}
