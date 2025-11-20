#pragma once

#include "VulkanBuffer.h"
#include "VulkanShadowMap.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

/**
 * VulkanLightingPass - Dark by default deferred lighting
 * 
 * Implements the core lighting philosophy:
 * - Dark by default - areas not hit by light are black
 * - Cascaded light maps (NOT shadow maps - inverted logic)
 * - 4 cascades: sun near, sun far, moon near, moon far
 * - 64-tap Poisson PCF soft lighting transitions
 * - Cloud shadow integration via 3D noise sampling
 * 
 * Performance-critical system - optimized for:
 * - Single fullscreen pass (no multi-pass overhead)
 * - Efficient cascade blending (only in transition zones)
 * - Minimal texture fetches (5 G-buffer + 1 light array)
 */
class VulkanLightingPass {
public:
    struct PushConstants {
        glm::vec4 sunDirection;     // xyz = direction, w = intensity
        glm::vec4 moonDirection;    // xyz = direction, w = intensity
        glm::vec4 sunColor;         // rgb = color, w = unused
        glm::vec4 moonColor;        // rgb = color, w = unused
        glm::vec4 cameraPos;        // xyz = position, w = timeOfDay
        glm::vec4 cascadeParams;    // x = ditherStrength, y = enableCloudShadows, zw = unused
    };

    struct CascadeUniforms {
        glm::mat4 cascadeVP[4];         // View-projection for each cascade
        glm::vec4 cascadeOrthoSizes;    // Ortho sizes for PCF radius scaling
        glm::vec4 lightTexel;           // x = 1/shadowMapSize, yzw = unused
    };

    VulkanLightingPass() = default;
    ~VulkanLightingPass() { destroy(); }

    // No copy
    VulkanLightingPass(const VulkanLightingPass&) = delete;
    VulkanLightingPass& operator=(const VulkanLightingPass&) = delete;

    /**
     * Initialize lighting pass
     * @param device Vulkan device
     * @param allocator VMA allocator
     * @param pipelineCache Pipeline cache for faster pipeline creation
     * @param gBufferDescriptorLayout Layout for G-buffer textures (set 0)
     * @param outputFormat Format of final output image
     * @param externalRenderPass Optional render pass to use (if null, creates own)
     */
    bool initialize(VkDevice device, VmaAllocator allocator, VkPipelineCache pipelineCache,
                    VkDescriptorSetLayout gBufferDescriptorLayout,
                    VkFormat outputFormat,
                    VkRenderPass externalRenderPass = VK_NULL_HANDLE);

    bool updateCloudNoiseDescriptor(VkImageView cloudNoiseTexture);
    
    /**
     * Update cascade uniforms (call every frame)
     */
    void updateCascadeUniforms(const CascadeUniforms& cascades);
    
    /**
     * Bind shadow map and cloud noise textures (call once after shadow map creation)
     */
    bool bindTextures(const VulkanShadowMap& shadowMap, VkImageView cloudNoiseTexture, VkImageView ssrTexture);

    /**
     * Render lighting pass
     * @param commandBuffer Command buffer to record into
     * @param gBufferDescriptorSet Descriptor set with G-buffer textures
     * @param cloudNoiseTexture 3D noise texture for cloud shadows
     * @param params Lighting parameters (push constants)
     */
    void render(VkCommandBuffer commandBuffer,
                VkDescriptorSet gBufferDescriptorSet,
                VkImageView cloudNoiseTexture,
                const PushConstants& params);

    void destroy();

    VkRenderPass getRenderPass() const { return m_renderPass; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }

private:
    bool createRenderPass();
    bool createPipeline();
    bool createDescriptorLayouts();
    bool createUniformBuffer();
    bool updateDescriptorSet(const VulkanShadowMap& shadowMap, VkImageView cloudNoiseTexture, VkImageView ssrTexture);

    VkShaderModule loadShaderModule(const std::string& filepath);

    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    VkFormat m_outputFormat = VK_FORMAT_UNDEFINED;

    // Descriptor layouts
    VkDescriptorSetLayout m_gBufferLayout = VK_NULL_HANDLE;  // External (set 0)
    VkDescriptorSetLayout m_lightingLayout = VK_NULL_HANDLE; // Shadow/cloud textures (set 1)

    // Pipeline
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    bool m_ownsRenderPass = true;  // If false, don't destroy render pass
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    // Shaders
    VkShaderModule m_vertShader = VK_NULL_HANDLE;
    VkShaderModule m_fragShader = VK_NULL_HANDLE;

    // Descriptor pool and set for shadow/cloud/cascade (set 1)
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_lightingDescriptorSet = VK_NULL_HANDLE;
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkSampler m_cloudNoiseSampler = VK_NULL_HANDLE;
    VkSampler m_ssrSampler = VK_NULL_HANDLE;
    
    // Cascade uniform buffer
    VulkanBuffer m_cascadeUniformBuffer;
};
