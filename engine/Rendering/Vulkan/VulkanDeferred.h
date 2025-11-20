#pragma once

#include "VulkanGBuffer.h"
#include "VulkanBuffer.h"
#include "VulkanLightingPass.h"
#include "VulkanShadowMap.h"
#include "VulkanSSR.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

/**
 * VulkanDeferred - Complete deferred rendering pipeline
 * 
 * Manages two-pass rendering:
 * 1. Geometry pass: Render voxel quads to G-buffer
 * 2. Lighting pass: Fullscreen quad reading G-buffer, applying lighting
 */
class VulkanDeferred {
public:
    struct LightingParams {
        glm::vec4 sunDirection;    // xyz = direction, w = intensity
        glm::vec4 moonDirection;   // xyz = direction, w = intensity
        glm::vec4 sunColor;        // rgb = color
        glm::vec4 moonColor;       // rgb = color
        glm::vec4 cameraPos;       // xyz = position
    };

    VulkanDeferred() = default;
    ~VulkanDeferred() { destroy(); }

    // No copy
    VulkanDeferred(const VulkanDeferred&) = delete;
    VulkanDeferred& operator=(const VulkanDeferred&) = delete;

    /**
     * Initialize deferred rendering pipeline
     * @param device Vulkan device
     * @param allocator VMA allocator
     * @param pipelineCache Pipeline cache for faster pipeline creation
     * @param swapchainFormat Format of swapchain images
     * @param swapchainRenderPass Render pass for final swapchain composition
     * @param width Render target width
     * @param height Render target height
     */
    bool initialize(VkDevice device, VmaAllocator allocator, VkPipelineCache pipelineCache,
                    VkFormat swapchainFormat, VkRenderPass swapchainRenderPass,
                    uint32_t width, uint32_t height, VkQueue graphicsQueue, VkCommandPool commandPool);

    /**
     * Resize render targets
     */
    bool resize(uint32_t width, uint32_t height);

    /**
     * Begin geometry pass (writes to G-buffer)
     * @param commandBuffer Command buffer to record into
     * @return True if ready to render geometry
     */
    void beginGeometryPass(VkCommandBuffer commandBuffer);

    /**
     * End geometry pass
     */
    void endGeometryPass(VkCommandBuffer commandBuffer);

    /**
     * Compute SSR (call after geometry pass, before lighting pass)
     */
    void computeSSR(VkCommandBuffer commandBuffer, VkImageView colorBuffer,
                    const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix);

    /**
     * Render lighting pass (reads G-buffer, writes to swapchain image)
     * @param commandBuffer Command buffer to record into
     * @param swapchainImageView Swapchain image to render to
     * @param params Lighting parameters
     * @param cascades Cascade uniform data for shadow maps
     * @param cloudNoiseTexture 3D cloud noise texture for shadows
     */
    void renderLightingPass(VkCommandBuffer commandBuffer, 
                            VkImageView swapchainImageView,
                            const LightingParams& params,
                            const VulkanLightingPass::CascadeUniforms& cascades,
                            VkImageView cloudNoiseTexture);

    /**
     * Bind shadow maps and cloud noise to lighting pass
     * Call once after shadow maps are created
     */
    bool bindLightingTextures(VkImageView cloudNoiseTexture);

    void destroy();

    // Accessors for geometry pass
    VkRenderPass getGeometryRenderPass() const { return m_gbuffer.getRenderPass(); }
    VkPipelineLayout getGeometryPipelineLayout() const { return m_geometryPipelineLayout; }
    VkDescriptorSetLayout getGeometryDescriptorLayout() const { return m_geometryDescriptorLayout; }
    VkDescriptorSet getGBufferDescriptorSet() const { return m_lightingDescriptorSet; }
    VkImageView getDepthView() const { return m_gbuffer.getDepthView(); }
    VkImageView getAlbedoView() const { return m_gbuffer.getAlbedoView(); }
    
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    
    VulkanShadowMap& getShadowMap() { return m_shadowMap; }
    VulkanSSR& getSSR() { return m_ssr; }

private:
    bool loadShaders();
    bool createGeometryPipeline();
    bool createDescriptorSetLayouts();
    bool createDescriptorSets();

    VkShaderModule loadShaderModule(const std::string& filepath);

    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // G-buffer
    VulkanGBuffer m_gbuffer;

    // Geometry pass resources
    VkShaderModule m_gbufferVertShader = VK_NULL_HANDLE;
    VkShaderModule m_gbufferFragShader = VK_NULL_HANDLE;
    VkPipelineLayout m_geometryPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_geometryPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_geometryDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_geometryDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_geometryDescriptorSet = VK_NULL_HANDLE;

    // Lighting pass resources (G-buffer descriptor set for shader access)
    VkDescriptorSetLayout m_lightingDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_lightingDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_lightingDescriptorSet = VK_NULL_HANDLE;
    VkSampler m_gbufferSampler = VK_NULL_HANDLE;

    // Advanced lighting system
    VulkanLightingPass m_lightingPass;
    VulkanShadowMap m_shadowMap;
    VulkanSSR m_ssr;
};
