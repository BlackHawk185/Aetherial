#pragma once

#include "VulkanContext.h"
#include <glm/glm.hpp>

class VulkanTriangleRenderer {
public:
    VulkanTriangleRenderer() = default;
    ~VulkanTriangleRenderer();
    
    bool init(VulkanContext* context);
    void cleanup();
    void render(VkCommandBuffer commandBuffer, float time);
    
private:
    bool createGraphicsPipeline();
    bool createVertexBuffer();
    VkShaderModule createShaderModule(const std::vector<uint32_t>& code);
    
    VulkanContext* m_context = nullptr;
    
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
    
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
    
    struct Vertex {
        glm::vec2 pos;
        glm::vec3 color;
    };
};
