// InstancedQuadRenderer.h - GPU instanced rendering for voxel quads
// Uses a single shared unit quad mesh, rendered multiple times with per-instance data
#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <cstdint>

class VoxelChunk;

class InstancedQuadRenderer
{
public:
    InstancedQuadRenderer();
    ~InstancedQuadRenderer();

    bool initialize();
    void shutdown();
    
    // Register a chunk for instanced rendering
    void registerChunk(VoxelChunk* chunk, const glm::mat4& transform);
    void registerChunkWithSize(VoxelChunk* chunk, const glm::mat4& transform, size_t estimatedQuads);
    
    // Update chunk transform (for moving islands)
    void updateChunkTransform(VoxelChunk* chunk, const glm::mat4& transform);
    
    void uploadChunkMesh(VoxelChunk* chunk);
    
    void renderToGBufferMDI(const glm::mat4& viewProjection, const glm::mat4& view);
    void renderToGBufferCulledMDI(const glm::mat4& viewProjection, const glm::mat4& view, const std::vector<VoxelChunk*>& visibleChunks);
    void renderToGBufferCulledMDI_GPU(const glm::mat4& viewProjection, const glm::mat4& view);
    
    // Light depth pass - batched MDI rendering
    void renderLightDepthMDI(const glm::mat4& lightVP, const std::vector<VoxelChunk*>& visibleChunks,
                            GLuint gbufferPositionTex, const glm::mat4& viewProj);
    
    // Clear all registered chunks
    void clear();

private:
    // Shared unit quad (uploaded once, used by all instances)
    GLuint m_unitQuadVAO;
    GLuint m_unitQuadVBO;
    GLuint m_unitQuadEBO;
    
    GLuint m_gbufferMDIProgram;
    GLint m_gbufferMDI_uViewProjection;
    GLint m_gbufferMDI_uBlockTextures;
    
    GLuint m_depthMDIProgram;
    GLint m_depthMDI_uLightVP;
    
    // MDI: DrawElementsIndirectCommand structure
    struct DrawElementsIndirectCommand {
        uint32_t count;         // Number of indices (always 6 for quad)
        uint32_t instanceCount; // Number of quads in this chunk
        uint32_t firstIndex;    // Starting index (always 0)
        uint32_t baseVertex;    // Base vertex offset (always 0)
        uint32_t baseInstance;  // Starting instance for this draw
    };
    
    struct ChunkEntry {
        VoxelChunk* chunk;
        glm::mat4 transform;
        size_t instanceCount;
        GLuint vbo = 0;
        size_t lastUploadedCount = 0;
        uint32_t chunkID = 0;
        uint32_t baseInstance = 0;  // Offset into unified instance buffer (in quads, not bytes)
        size_t allocatedSlots = 0;   // Number of slots reserved for this chunk (with padding)
        bool needsGPUSync = false;
    };
    
    std::vector<ChunkEntry> m_chunks;
    std::unordered_map<VoxelChunk*, size_t> m_chunkToIndex;
    
    GLuint m_transformSSBO;
    GLuint m_blockTextureArray;
    GLuint m_mdiCommandBuffer;
    GLuint m_mdiInstanceBuffer;
    GLuint m_mdiVAO;
    
    // Persistent mapped buffers (GL 4.4+)
    GLuint m_persistentQuadBuffer;
    void* m_persistentQuadPtr = nullptr;
    size_t m_persistentQuadCapacity = 0;
    size_t m_persistentQuadUsed = 0;  // Track allocated space
    
    // Persistent buffers for commands and transforms
    GLuint m_persistentCommandBuffer;
    void* m_persistentCommandPtr = nullptr;
    GLuint m_persistentTransformBuffer;
    void* m_persistentTransformPtr = nullptr;
    
    // VBO pool for reuse
    std::vector<GLuint> m_freeVBOPool;
    GLuint allocateVBO(size_t sizeBytes);
    void freeVBO(GLuint vbo);
    
    // GPU frustum culling
    GLuint m_frustumCullProgram;
    GLuint m_visibilitySSBO;
    void createFrustumCullShader();
    void cullChunksGPU(const glm::mat4& viewProj, std::vector<VoxelChunk*>& outVisible);
    
    // Helper methods
    void createUnitQuad();
    void createShader();
    void createGBufferShader();
    void createDepthShader();
    bool loadBlockTextureArray();  // Load all block textures into texture array
    GLuint compileShader(const char* source, GLenum type);
    void uploadChunkInstances(ChunkEntry& entry);
    void updateSingleChunkGPU(ChunkEntry& entry);  // Upload new quads to per-chunk VBO
};

// Global instance
extern std::unique_ptr<InstancedQuadRenderer> g_instancedQuadRenderer;
