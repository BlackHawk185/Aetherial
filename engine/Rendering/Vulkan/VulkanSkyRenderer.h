// VulkanSkyRenderer.h - Vulkan port of SkyRenderer
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>

class VulkanContext;
class VulkanBuffer;

/**
 * Vulkan Sky Renderer
 * 
 * Renders a complete skybox cube with:
 * - Dynamic sky gradients (day/night/sunset transitions)
 * - Animated starfield during night
 * - Realistic sun disc with glow effects
 * - Proper depth handling to render behind all geometry
 */
class VulkanSkyRenderer {
public:
    VulkanSkyRenderer();
    ~VulkanSkyRenderer();

    bool initialize(VulkanContext* ctx);
    void shutdown();

    /**
     * Render skybox cube to current render pass
     * @param cmd Command buffer to record commands into
     * @param sunDirection Direction TO the sun (normalized)
     * @param sunIntensity 0.0 (night) to 1.0 (day)
     * @param moonDirection Direction TO the moon (normalized)
     * @param moonIntensity 0.0 (day) to 1.0 (night)
     * @param viewMatrix Camera view matrix
     * @param projectionMatrix Camera projection matrix
     * @param timeOfDay Time value for star twinkling animation
     */
    void render(VkCommandBuffer cmd,
               const glm::vec3& sunDirection, float sunIntensity, 
               const glm::vec3& moonDirection, float moonIntensity,
               const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, 
               float timeOfDay);

    // Sky appearance parameters
    void setSunSize(float size) { m_sunSize = size; }
    void setSunGlow(float glow) { m_sunGlow = glow; }
    void setMoonSize(float size) { m_moonSize = size; }
    void setExposure(float exposure) { m_exposure = exposure; }

private:
    struct PushConstants {
        glm::mat4 viewProj;
        glm::vec3 sunDir;
        float sunIntensity;
        glm::vec3 moonDir;
        float moonIntensity;
        glm::vec3 cameraPos;
        float timeOfDay;
        float sunSize;
        float sunGlow;
        float moonSize;
        float exposure;
    };

    bool createShaders();
    bool createGeometry();
    bool createPipeline();

    VulkanContext* m_context = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;

    // Geometry buffers
    std::unique_ptr<VulkanBuffer> m_vertexBuffer;
    std::unique_ptr<VulkanBuffer> m_indexBuffer;

    // Pipeline objects
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_vertexShader = VK_NULL_HANDLE;
    VkShaderModule m_fragmentShader = VK_NULL_HANDLE;

    // Sky parameters
    float m_sunSize = 0.1f;
    float m_sunGlow = 4.0f;
    float m_moonSize = 0.08f;
    float m_exposure = 1.0f;

    bool m_initialized = false;
};
