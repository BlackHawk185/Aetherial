// InstancedQuadRenderer.h - GPU instanced rendering for voxel quads
// Uses a single shared unit quad mesh, rendered multiple times with per-instance data
#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <vector>
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
    
    // Update chunk transform (for moving islands)
    void updateChunkTransform(VoxelChunk* chunk, const glm::mat4& transform);
    
    // Rebuild chunk instance data (call when mesh changes)
    void rebuildChunk(VoxelChunk* chunk);
    
    // Render all registered chunks with CSM/PCF shadows
    void render(const glm::mat4& viewProjection, const glm::mat4& view);
    
    // Shadow depth pass (for casting shadows)
    void beginDepthPass(const glm::mat4& lightVP, int cascadeIndex);
    void renderDepth();
    void endDepthPass(int screenWidth, int screenHeight);
    
    // Clear all registered chunks
    void clear();

private:
    // Shared unit quad (uploaded once, used by all instances)
    GLuint m_unitQuadVAO;
    GLuint m_unitQuadVBO;
    GLuint m_unitQuadEBO;
    
    // Shader program
    GLuint m_shaderProgram;
    
    // Depth-only shader for shadow map rendering
    GLuint m_depthProgram;
    GLint m_depth_uLightVP;
    
    // Uniform locations
    GLint m_uViewProjection;
    GLint m_uView;
    GLint m_uTextureStone;
    GLint m_uTextureDirt;
    GLint m_uTextureGrass;
    GLint m_uTextureSand;
    
    // CSM/PCF shadow uniforms
    GLint m_uShadowMap;
    GLint m_uShadowTexel;
    GLint m_uLightDir;
    GLint m_uCascadeVP;
    
    // MDI: DrawElementsIndirectCommand structure
    struct DrawElementsIndirectCommand {
        uint32_t count;         // Number of indices (always 6 for quad)
        uint32_t instanceCount; // Number of quads in this chunk
        uint32_t firstIndex;    // Starting index (always 0)
        uint32_t baseVertex;    // Base vertex offset (always 0)
        uint32_t baseInstance;  // Starting instance for this draw
    };
    
    // Chunk registration
    struct ChunkEntry {
        VoxelChunk* chunk;
        glm::mat4 transform;
        size_t instanceCount;
        size_t baseInstance;    // Offset into merged instance buffer
        size_t allocatedSlots;  // Pre-allocated buffer space for this chunk
    };
    
    std::vector<ChunkEntry> m_chunks;
    
    // MDI buffers
    GLuint m_globalVAO;              // Single VAO for all chunks
    GLuint m_globalInstanceVBO;      // Merged instance data for all chunks
    GLuint m_indirectCommandBuffer;  // GPU buffer for draw commands
    GLuint m_transformSSBO;          // Chunk transforms for shader lookup
    
    bool m_mdiDirty;                 // True when buffers need rebuild
    size_t m_totalAllocatedInstances; // Total buffer capacity (with padding)
    
    // Helper methods
    void createUnitQuad();
    void createShader();
    void createDepthShader();
    GLuint compileShader(const char* source, GLenum type);
    void uploadChunkInstances(ChunkEntry& entry);
    void rebuildMDIBuffers();  // Rebuild merged buffers for MDI (full)
    void updateSingleChunkGPU(ChunkEntry& entry);  // Partial update for one chunk
    size_t calculateChunkSlots(size_t quadCount);  // Calculate padded allocation size
};

// Global instance
extern std::unique_ptr<InstancedQuadRenderer> g_instancedQuadRenderer;
