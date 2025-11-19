#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include "VulkanBuffer.h"
#include <memory>

class VulkanContext;

/**
 * VulkanCloudRenderer - Volumetric cloud rendering for Vulkan
 * 
 * Renders realistic volumetric clouds using raymarching through a 3D noise texture.
 * Designed to integrate into deferred pipeline after lighting pass.
 * 
 * Features:
 * - 3D Perlin/Worley noise for cloud density
 * - Altitude-based cloud layer
 * - Beer-Lambert light absorption
 * - Sun lighting integration
 * - Configurable density, coverage, and detail
 */
class VulkanCloudRenderer {
public:
    struct CloudParams {
        glm::vec3 sunDirection;
        float sunIntensity;
        glm::vec3 cameraPosition;
        float timeOfDay;
        float cloudCoverage;
        float cloudDensity;
        float cloudSpeed;
    };

    VulkanCloudRenderer() = default;
    ~VulkanCloudRenderer() { destroy(); }

    // No copy
    VulkanCloudRenderer(const VulkanCloudRenderer&) = delete;
    VulkanCloudRenderer& operator=(const VulkanCloudRenderer&) = delete;

    /**
     * Initialize cloud renderer
     * @param context Vulkan context (provides render pass)
     * @param width Render target width
     * @param height Render target height
     */
    bool initialize(VulkanContext* context, uint32_t width, uint32_t height);

    /**
     * Resize render targets
     */
    bool resize(uint32_t width, uint32_t height);

    /**
     * Render volumetric clouds (renders inside active render pass)
     * @param cmd Command buffer (must be inside render pass)
     * @param depthTexture Scene depth texture to sample
     * @param viewMatrix Camera view matrix
     * @param projectionMatrix Camera projection matrix
     * @param params Cloud rendering parameters
     */
    void render(VkCommandBuffer cmd, VkImageView depthTexture,
                const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                const CloudParams& params);

    void destroy();

    // Parameter setters
    void setCloudCoverage(float coverage) { m_cloudCoverage = coverage; }
    void setCloudDensity(float density) { m_cloudDensity = density; }
    void setCloudSpeed(float speed) { m_cloudSpeed = speed; }

    // Get 3D noise texture for external use (shadow rendering)
    VkImage getNoiseTexture() const { return m_noiseTexture; }
    VkImageView getNoiseTextureView() const { return m_noiseTextureView; }

private:
    bool loadShaders();
    bool createPipeline();
    bool create3DNoiseTexture();
    bool createDescriptorSet();

    VkShaderModule loadShaderModule(const char* filepath);

    VulkanContext* m_context = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;  // Borrowed from context
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // Pipeline resources
    VkShaderModule m_vertShader = VK_NULL_HANDLE;
    VkShaderModule m_fragShader = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    // Descriptor resources
    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkSampler m_depthSampler = VK_NULL_HANDLE;
    VkSampler m_noiseSampler = VK_NULL_HANDLE;

    // 3D noise texture
    VkImage m_noiseTexture = VK_NULL_HANDLE;
    VkImageView m_noiseTextureView = VK_NULL_HANDLE;
    VmaAllocation m_noiseAllocation = VK_NULL_HANDLE;

    // Cloud parameters
    float m_cloudCoverage = 0.5f;
    float m_cloudDensity = 0.5f;
    float m_cloudSpeed = 0.5f;
};
