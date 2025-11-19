// VulkanBlockHighlighter.h - Vulkan wireframe cube for selected block
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include "VulkanBuffer.h"
#include <memory>

class VulkanContext;
struct Vec3;

class VulkanBlockHighlighter {
public:
    VulkanBlockHighlighter();
    ~VulkanBlockHighlighter();

    bool initialize(VulkanContext* ctx);
    void shutdown();
    
    // Render wireframe cube at block position (island-relative)
    void render(VkCommandBuffer cmd, const Vec3& blockPos, const glm::mat4& islandTransform, 
                const glm::mat4& viewProjection);

private:
    VulkanContext* m_context = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    
    // Buffers
    std::unique_ptr<VulkanBuffer> m_vertexBuffer;
    std::unique_ptr<VulkanBuffer> m_indexBuffer;
    
    // Pipeline
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    
    // Shaders
    VkShaderModule m_vertexShader = VK_NULL_HANDLE;
    VkShaderModule m_fragmentShader = VK_NULL_HANDLE;
    
    bool createBuffers();
    bool createShaders();
    bool createPipeline();
};
