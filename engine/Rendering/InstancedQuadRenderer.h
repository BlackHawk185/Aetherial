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
    GLint m_uTexture;
    GLint m_uStoneTexture;
    GLint m_uGrassTexture;
    GLint m_uSandTexture;
    
    // CSM/PCF shadow uniforms
    GLint m_uShadowMap;
    GLint m_uShadowTexel;
    GLint m_uLightDir;
    GLint m_uCascadeVP;
    
    // Chunk registration
    struct ChunkEntry {
        VoxelChunk* chunk;
        glm::mat4 transform;
        GLuint instanceVBO;  // Per-chunk instance buffer
        size_t instanceCount;
    };
    
    std::vector<ChunkEntry> m_chunks;
    
    // Helper methods
    void createUnitQuad();
    void createShader();
    void createDepthShader();
    GLuint compileShader(const char* source, GLenum type);
    void uploadChunkInstances(ChunkEntry& entry);
};

// Global instance
extern std::unique_ptr<InstancedQuadRenderer> g_instancedQuadRenderer;
