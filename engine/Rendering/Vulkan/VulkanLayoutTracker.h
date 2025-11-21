#pragma once
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <string>
#include <iostream>

// Debug tool to track image layout transitions and detect mismatches
class VulkanLayoutTracker {
private:
    std::unordered_map<VkImage, VkImageLayout> m_layouts;
    
public:
    bool m_verbose = false;  // Enable verbose to see tracking output
    
    static VulkanLayoutTracker& getInstance() {
        static VulkanLayoutTracker instance;
        return instance;
    }

    void recordTransition(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, const char* location) {
        m_layouts[image] = newLayout;
        // Only log if verbose mode enabled
        if (m_verbose) {
            std::cout << "[LayoutTracker] " << location << ": " 
                      << getLayoutName(oldLayout) << " -> " << getLayoutName(newLayout) 
                      << " (image=" << image << ")" << std::endl;
        }
    }

    void recordDescriptorWrite(VkImageView view, VkImageLayout declaredLayout, const char* location) {
        // Only log if verbose mode enabled
        if (m_verbose) {
            std::cout << "[LayoutTracker] Descriptor write at " << location 
                      << ": declared layout=" << getLayoutName(declaredLayout)
                      << " (view=" << view << ")" << std::endl;
        }
    }

    void recordRenderPassBegin(VkImage image, VkImageLayout layout, const char* passName) {
        VkImageLayout currentLayout = m_layouts[image];
        
        // ALWAYS log mismatches, regardless of verbose setting
        if (currentLayout != layout && currentLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            std::cerr << "[LayoutTracker] ⚠️  MISMATCH in " << passName 
                      << ": expects " << getLayoutName(layout)
                      << ", but current is " << getLayoutName(currentLayout)
                      << " (image=" << image << ")" << std::endl;
        }
        
        // Only log normal pass begins if verbose
        if (m_verbose && (currentLayout == layout || currentLayout == VK_IMAGE_LAYOUT_UNDEFINED)) {
            std::cout << "[LayoutTracker] Begin " << passName 
                      << ": expects " << getLayoutName(layout)
                      << ", current is " << getLayoutName(currentLayout)
                      << " (image=" << image << ")" << std::endl;
        }
    }

    VkImageLayout getCurrentLayout(VkImage image) const {
        auto it = m_layouts.find(image);
        return (it != m_layouts.end()) ? it->second : VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void setVerbose(bool verbose) { m_verbose = verbose; }
    bool isVerbose() const { return m_verbose; }

    const char* getLayoutName(VkImageLayout layout) const {
        switch (layout) {
            case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
            case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC_OPTIMAL";
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
            case VK_IMAGE_LAYOUT_PREINITIALIZED: return "PREINITIALIZED";
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT_SRC_KHR";
            default: return "UNKNOWN";
        }
    }
};

// RAII helper to automatically track transitions
class ScopedLayoutTransition {
public:
    ScopedLayoutTransition(VkCommandBuffer cmd, VkImage image, 
                          VkImageLayout oldLayout, VkImageLayout newLayout,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                          const char* location)
        : m_cmd(cmd), m_image(image), m_newLayout(newLayout) 
    {
        VulkanLayoutTracker::getInstance().recordTransition(image, oldLayout, newLayout, location);
        
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

private:
    VkCommandBuffer m_cmd;
    VkImage m_image;
    VkImageLayout m_newLayout;
};
