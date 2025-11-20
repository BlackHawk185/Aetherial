#pragma once

#include "VulkanBuffer.h"
#include "VulkanImage.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

class VulkanSSR {
public:
    struct PushConstants {
        glm::mat4 viewMatrix;
        glm::mat4 projectionMatrix;
        glm::mat4 invViewMatrix;
        glm::mat4 invProjectionMatrix;
    };

    VulkanSSR() = default;
    ~VulkanSSR() { destroy(); }

    VulkanSSR(const VulkanSSR&) = delete;
    VulkanSSR& operator=(const VulkanSSR&) = delete;

    bool initialize(VkDevice device, VmaAllocator allocator, VkPipelineCache pipelineCache,
                    uint32_t width, uint32_t height, VkQueue graphicsQueue, VkCommandPool commandPool);
    bool resize(uint32_t width, uint32_t height);
    void destroy();

    void compute(VkCommandBuffer cmd, 
                 VkImageView gNormal, VkImageView gPosition, VkImageView gDepth, 
                 VkImageView gMetadata, VkImageView colorBuffer,
                 const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix);

    VkImageView getOutputView() const { return m_reflectionImage.getView(); }

private:
    bool createPipeline();
    bool createDescriptorSet();
    VkShaderModule loadShaderModule(const std::string& filepath);

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
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    bool m_imageNeedsTransition = true;
};
