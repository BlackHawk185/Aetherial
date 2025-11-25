#pragma once

#include "VulkanGBuffer.h"
#include "VulkanBuffer.h"
#include "VulkanShadowMap.h"
#include "VulkanSSPR.h"
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
                    VkFormat swapchainFormat,
                    uint32_t width, uint32_t height, VkQueue graphicsQueue, VkCommandPool commandPool,
                    class VulkanContext* context);

    /**
     * Resize render targets
     */
    bool resize(uint32_t width, uint32_t height);

    /**
     * Begin geometry pass (writes to G-buffer)
     * @param commandBuffer Command buffer to record into
     * @param depthImage External depth image (from VulkanContext)
     * @param depthView External depth view (from VulkanContext)
     * @param depthLayout Current layout of depth image
     */
    void beginGeometryPass(VkCommandBuffer commandBuffer, VkImage depthImage, VkImageView depthView, VkImageLayout depthLayout);

    /**
     * End geometry pass
     */
    void endGeometryPass(VkCommandBuffer commandBuffer);

    /**
     * Compute SSR (call AFTER lighting pass - raymarches HDR buffer)
     */
    void computeSSR(VkCommandBuffer commandBuffer,
                    const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix);

    // Cascade uniforms (moved from VulkanLightingPass)
    struct CascadeUniforms {
        glm::mat4 cascadeVP[4];         // View-projection for each cascade
        glm::vec4 cascadeOrthoSizes;    // Ortho sizes for PCF radius scaling
        glm::vec4 lightTexel;           // x = 1/shadowMapSize, yzw = unused
    };

    /**
     * Render lighting pass (reads G-buffer, writes to HDR buffer)
     * Call this before SSR - renders all non-water geometry
     */
    void renderLightingToHDR(VkCommandBuffer commandBuffer,
                             const LightingParams& params,
                             const CascadeUniforms& cascades,
                             VkImageView cloudNoiseTexture);
    
    /**
     * Composite HDR + SSR to swapchain
     * Call this after SSR, inside swapchain render pass
     */
    void compositeToSwapchain(VkCommandBuffer commandBuffer, const glm::vec3& cameraPos);
    
    /**
     * Legacy: Render lighting pass (reads G-buffer, writes to swapchain image)
     * @param commandBuffer Command buffer to record into
     * @param swapchainImageView Swapchain image to render to
     * @param params Lighting parameters
     * @param cascades Cascade uniform data for shadow maps
     * @param cloudNoiseTexture 3D cloud noise texture for shadows
     */
    void renderLightingPass(VkCommandBuffer commandBuffer, 
                            VkImageView swapchainImageView,
                            const LightingParams& params,
                            const CascadeUniforms& cascades,
                            VkImageView cloudNoiseTexture);

    /**
     * Bind shadow maps and cloud noise to lighting pass
     * Call once after shadow maps are created
     */
    bool bindLightingTextures(VkImageView cloudNoiseTexture);

    void destroy();

    // Accessors for geometry pass
    VkPipelineLayout getGeometryPipelineLayout() const { return m_geometryPipelineLayout; }
    VkDescriptorSetLayout getGeometryDescriptorLayout() const { return m_geometryDescriptorLayout; }
    VkDescriptorSet getGBufferDescriptorSet() const { return m_lightingDescriptorSets[0]; }  // Backward compat, use frame 0
    VkImageView getAlbedoView() const { return m_gbuffer.getAlbedoView(); }
    VkImageView getHDRView() const { return m_hdrBuffer.getView(); }
    
    // Accessors for dynamic descriptor updates
    VkDescriptorSet getLightingDescriptorSet(uint32_t frameIndex) const { return m_lightingDescriptorSets[frameIndex % MAX_FRAMES_IN_FLIGHT]; }
    VkSampler getGBufferSampler() const { return m_gbufferSampler; }
    // Depth view is from VulkanContext, not G-buffer

    
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    
    VulkanShadowMap& getShadowMap() { return m_shadowMap; }
    VulkanSSPR& getSSPR() { return m_sspr; }

private:
    bool loadShaders();
    bool createGeometryPipeline();
    bool createLightingPipeline();
    bool createDescriptorSetLayouts();
    bool createDescriptorSets();
    bool createCompositePipeline();

    VkShaderModule loadShaderModule(const std::string& filepath);

    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
    class VulkanContext* m_context = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // G-buffer
    VulkanGBuffer m_gbuffer;
    
    // HDR buffer for intermediate rendering (enables SSR)
    VulkanImage m_hdrBuffer;


    // Geometry pass resources
    VkShaderModule m_gbufferVertShader = VK_NULL_HANDLE;
    VkShaderModule m_gbufferFragShader = VK_NULL_HANDLE;
    VkPipelineLayout m_geometryPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_geometryPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_geometryDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_geometryDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_geometryDescriptorSet = VK_NULL_HANDLE;

    // Lighting pass resources - renders ALL geometry including water
    VkShaderModule m_lightingVertShader = VK_NULL_HANDLE;
    VkShaderModule m_lightingFragShader = VK_NULL_HANDLE;
    VkPipelineLayout m_lightingPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_lightingPipeline = VK_NULL_HANDLE;
    
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
    VkDescriptorSetLayout m_lightingDescriptorLayout = VK_NULL_HANDLE;  // Set 0: G-buffer
    VkDescriptorPool m_lightingDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_lightingDescriptorSets[MAX_FRAMES_IN_FLIGHT] = {};  // Set 0: G-buffer textures (per-frame)
    VkSampler m_gbufferSampler = VK_NULL_HANDLE;
    
    VkDescriptorSetLayout m_shadowDescriptorLayout = VK_NULL_HANDLE;  // Set 1: Shadows/noise
    VkDescriptorPool m_shadowDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_shadowDescriptorSet = VK_NULL_HANDLE;
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkSampler m_cloudNoiseSampler = VK_NULL_HANDLE;
    VkSampler m_ssrSampler = VK_NULL_HANDLE;
    
    VulkanBuffer m_cascadeUniformBuffer;  // Cascade matrices/params

    // Advanced rendering systems
    VulkanShadowMap m_shadowMap;
    VulkanSSPR m_sspr;
    
    // Composite pass (HDR + SSR blend)
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_compositePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compositeDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_compositeDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_compositeDescriptorSets[MAX_FRAMES_IN_FLIGHT] = {};
    VkSampler m_compositeSampler = VK_NULL_HANDLE;
};
