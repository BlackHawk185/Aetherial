// VulkanModelRenderer.h - Instanced GLB model rendering with fallback magenta cubes
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include "VulkanBuffer.h"
#include "../../Assets/GLBLoader.h"

class VoxelChunk;
class VulkanContext;

class VulkanModelRenderer {
public:
    VulkanModelRenderer();
    ~VulkanModelRenderer();

    bool initialize(VulkanContext* ctx, VkRenderPass gbufferRenderPass);
    void shutdown();

    // Model loading (called once per block type)
    bool loadModel(uint8_t blockID, const std::string& glbPath);
    
    // Chunk management - called when chunks update their model instances
    void updateChunkInstances(VoxelChunk* chunk, uint32_t islandID, const glm::vec3& chunkOffset);
    void unregisterChunk(VoxelChunk* chunk);
    
    // Island transform updates
    void updateIslandTransform(uint32_t islandID, const glm::mat4& transform);
    
    // Rendering
    void renderToGBuffer(VkCommandBuffer cmd, const glm::mat4& viewProjection, const glm::mat4& view);

private:
    struct ModelData {
        std::unique_ptr<VulkanBuffer> vertexBuffer;
        std::unique_ptr<VulkanBuffer> indexBuffer;
        uint32_t indexCount;
        bool isFallback;  // True if this is a magenta cube placeholder
    };
    
    struct InstanceData {
        glm::vec3 position;
        uint32_t islandID;
    };
    
    struct DrawBatch {
        uint8_t blockID;
        std::vector<InstanceData> instances;
        bool needsUpload;
    };

    VulkanContext* m_context = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    
    // Model cache - one entry per BlockID that uses OBJ render type
    std::unordered_map<uint8_t, ModelData> m_models;
    
    // Per-chunk instance tracking
    std::unordered_map<VoxelChunk*, std::vector<DrawBatch>> m_chunkBatches;
    
    // Unified instance buffer (all models share this, rebuilt each frame)
    std::unique_ptr<VulkanBuffer> m_instanceBuffer;
    std::vector<InstanceData> m_instanceData;
    
    // Island transforms
    std::unique_ptr<VulkanBuffer> m_islandTransformBuffer;
    std::unordered_map<uint32_t, glm::mat4> m_islandTransforms;
    
    // Pipeline and descriptors
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkRenderPass m_gbufferRenderPass = VK_NULL_HANDLE;
    
    // Helper functions
    void createMagentaCube(uint8_t blockID);
    void createPipeline();
    void createDescriptors();
    void rebuildInstanceBuffer();
};
