#pragma once

#include "VulkanImage.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>

/**
 * VulkanShadowMap - Cascaded shadow mapping for directional lights
 * 
 * Creates a 2D image array with depth attachments for multiple cascade layers.
 * Each cascade covers a different distance range for improved shadow detail.
 */
class VulkanShadowMap {
public:
    struct CascadeData {
        glm::mat4 viewProj;
        float splitDistance;
        float orthoSize;
    };

    VulkanShadowMap() = default;
    ~VulkanShadowMap() { destroy(); }

    // No copy
    VulkanShadowMap(const VulkanShadowMap&) = delete;
    VulkanShadowMap& operator=(const VulkanShadowMap&) = delete;

    /**
     * Initialize shadow map system
     * @param device Vulkan device
     * @param allocator VMA allocator
     * @param size Shadow map resolution (e.g., 4096x4096)
     * @param numCascades Number of cascade layers (typically 4)
     */
    bool initialize(VkDevice device, VmaAllocator allocator, 
                    uint32_t size = 4096, uint32_t numCascades = 4);

    /**
     * Resize shadow maps
     */
    bool resize(uint32_t newSize);

    /**
     * Begin rendering to specific cascade
     * @param commandBuffer Command buffer to record into
     * @param cascadeIndex Which cascade to render (0-3)
     */
    void beginCascadeRender(VkCommandBuffer commandBuffer, uint32_t cascadeIndex);

    /**
     * End cascade rendering and transition for shader access
     */
    void endCascadeRender(VkCommandBuffer commandBuffer, uint32_t cascadeIndex);

    /**
     * Transition all cascades to shader read layout
     */
    void transitionForShaderRead(VkCommandBuffer commandBuffer);

    void destroy();

    // Accessors
    VkRenderPass getRenderPass() const { return m_renderPass; }
    VkFramebuffer getFramebuffer(uint32_t cascadeIndex) const { 
        return m_framebuffers[cascadeIndex]; 
    }
    VkImageView getView() const { return m_shadowImage.getView(); }
    VkImageView getShadowMapImageView() const { return m_shadowImage.getView(); }
    VkSampler getShadowSampler() const { return m_shadowSampler; }
    
    uint32_t getSize() const { return m_size; }
    uint32_t getNumCascades() const { return m_numCascades; }

    const CascadeData& getCascade(uint32_t index) const { return m_cascades[index]; }
    void setCascadeData(uint32_t index, const CascadeData& data) { m_cascades[index] = data; }

private:
    bool createShadowImage();
    bool createRenderPass();
    bool createFramebuffers();
    bool createSampler();

    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    uint32_t m_size = 0;
    uint32_t m_numCascades = 0;

    // Shadow map image array (depth texture with multiple layers)
    VulkanImage m_shadowImage;
    
    // Per-cascade framebuffers and image views
    std::vector<VkImageView> m_cascadeImageViews;
    std::vector<VkFramebuffer> m_framebuffers;
    
    // Render pass for depth-only rendering
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    
    // Shadow sampler (with PCF and border clamp)
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    
    // Cascade data
    std::vector<CascadeData> m_cascades;
};