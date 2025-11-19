// VulkanQuadRenderer.h - Vulkan port of InstancedQuadRenderer
// Phase 2: Instanced quad rendering with MDI and architecture-aware buffer management
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <memory>
#include "VulkanBuffer.h"

class VoxelChunk;
class VulkanContext;

class VulkanQuadRenderer {
public:
    VulkanQuadRenderer();
    ~VulkanQuadRenderer();

    // Initialize with Vulkan context
    bool initialize(VulkanContext* ctx);
    void shutdown();

    // Chunk management
    void registerChunk(VoxelChunk* chunk, uint32_t islandID, const glm::vec3& chunkOffset);
    void unregisterChunk(VoxelChunk* chunk);
    void uploadChunkMesh(VoxelChunk* chunk);
    
    // Island transform updates (call when islands move)
    void updateIslandTransform(uint32_t islandID, const glm::mat4& transform);

    // Buffer updates (call before beginFrame)
    void updateDynamicBuffers(VkCommandBuffer cmd, const glm::mat4& viewProjection);
    
    // Process pending uploads (call once per frame, batches all uploads)
    void processPendingUploads();

    // Rendering
    void renderToGBuffer(VkCommandBuffer cmd, const glm::mat4& viewProjection, const glm::mat4& view);
    
    // Depth-only rendering for shadow cascades (Phase 4)
    void renderDepthOnly(VkCommandBuffer cmd, VkRenderPass shadowRenderPass, const glm::mat4& lightViewProjection);
    
    // Phase 2 testing: render to swapchain (simplified, no G-buffer)
    void renderToSwapchain(VkCommandBuffer cmd, const glm::mat4& viewProjection, const glm::mat4& view);

    void clear();

private:
    struct ChunkEntry {
        VoxelChunk* chunk;
        uint32_t islandID;
        glm::vec3 chunkOffset;
        size_t instanceCount;
        uint32_t baseInstance;      // Offset into unified instance buffer
        size_t allocatedSlots;      // Number of slots reserved (includes 25% padding for quad explosion)
        bool needsGPUSync;
    };

    VulkanContext* m_context = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;

    // Chunk tracking
    std::vector<ChunkEntry> m_chunks;
    std::unordered_map<VoxelChunk*, size_t> m_chunkToIndex;

    // GPU Architecture detection
    bool m_isIntegratedGPU = false;
    bool m_hasHostVisibleDeviceLocal = false;

    // Buffers
    std::unique_ptr<VulkanBuffer> m_unitQuadVertexBuffer;  // Static quad vertices
    std::unique_ptr<VulkanBuffer> m_unitQuadIndexBuffer;   // Static quad indices
    std::unique_ptr<VulkanBuffer> m_instanceBuffer;        // QuadFace data (64MB persistent) - vertex pulling
    std::unique_ptr<VulkanBuffer> m_islandTransformBuffer; // Per-island mat4 transforms (SSBO)
    
    // Island transform tracking for dynamic updates
    std::unordered_map<uint32_t, glm::mat4> m_islandTransforms;
    std::vector<uint32_t> m_islandIDList;

    size_t m_instanceBufferCapacity = 0;
    size_t m_instanceBufferUsed = 0;
    
    // Pending uploads (batched per frame)
    std::vector<ChunkEntry*> m_pendingUploads;

    // Pipeline and descriptors
    VkPipeline m_gbufferPipeline = VK_NULL_HANDLE;
    VkPipeline m_swapchainPipeline = VK_NULL_HANDLE;  // Phase 2 testing: simple swapchain pipeline
    VkPipeline m_depthOnlyPipeline = VK_NULL_HANDLE;  // Phase 4: depth-only for shadow cascades
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkRenderPass m_gbufferRenderPass = VK_NULL_HANDLE;

    // Shader modules
    VkShaderModule m_vertexShader = VK_NULL_HANDLE;
    VkShaderModule m_fragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_fragmentShaderSimple = VK_NULL_HANDLE;  // Phase 2 testing: single-output fragment shader

    // Texture array
    VkImage m_blockTextureArray = VK_NULL_HANDLE;
    VkImageView m_blockTextureArrayView = VK_NULL_HANDLE;
    VkSampler m_blockTextureSampler = VK_NULL_HANDLE;
    VmaAllocation m_blockTextureAllocation = VK_NULL_HANDLE;

    // Helper methods
    void detectGPUArchitecture();
    void createUnitQuad();
    void uploadUnitQuadData();
    void createShaders();
    void createDescriptorSetLayout();
    void createPipeline();
    void createSwapchainPipeline();  // Phase 2 testing
    void createDepthOnlyPipeline();  // Phase 4: shadow map rendering (stub)
    void ensureDepthPipeline(VkRenderPass shadowRenderPass);  // Lazy creation with render pass
    void createDescriptorPool();
    void updateDescriptorSets();
    bool loadBlockTextureArray();
    bool createPlaceholderTexture();
    void uploadInstanceData(ChunkEntry& entry);
};
