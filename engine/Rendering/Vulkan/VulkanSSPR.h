#pragma once

#include "VulkanBuffer.h"
#include "VulkanImage.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

class VulkanSSPR {
public:
    struct PushConstants {
        glm::mat4 viewMatrix;
        glm::mat4 projectionMatrix;
        glm::vec3 cameraPos;
        float planeY;
    };

    VulkanSSPR() = default;
    ~VulkanSSPR() { destroy(); }

    VulkanSSPR(const VulkanSSPR&) = delete;
    VulkanSSPR& operator=(const VulkanSSPR&) = delete;

    bool initialize(VkDevice device, VmaAllocator allocator, VkPipelineCache pipelineCache,
                    uint32_t width, uint32_t height, VkQueue graphicsQueue, VkCommandPool commandPool);
    bool resize(uint32_t width, uint32_t height);
    void destroy();

    void compute(VkCommandBuffer cmd, 
                 VkImageView gNormal, VkImageView gPosition, VkImageView gDepth, 
                 VkImageView gMetadata, VkImageView hdrBuffer,
                 const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, 
                 const glm::vec3& cameraPos, float time, uint32_t frameIndex);

    VkImageView getOutputView() const { return m_reflectionImage.getView(); }

private:
    bool createPipeline();
    bool createDescriptorSet();
    VkShaderModule loadShaderModule(const std::string& filepath);

    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
    
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    VulkanImage m_reflectionImage;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkShaderModule m_computeShader = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSets[MAX_FRAMES_IN_FLIGHT] = {};
    bool m_imageNeedsTransition = true;
};
