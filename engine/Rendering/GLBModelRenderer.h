// GLBModelRenderer.h - MDI instanced rendering for GLB models (grass, water, etc.)
// Grid-aligned block models that inherit chunk transforms via shared SSBO
#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <string>
#include "../Math/Vec3.h"
#include "../Assets/GLBLoader.h"

class VoxelChunk;

// Per-model GPU resources (each primitive in GLB gets separate VAO/VBO/EBO)
struct GLBPrimitiveGPU {
    GLuint vao = 0;
    GLuint vbo = 0;      // Interleaved: pos(3), normal(3), uv(2)
    GLuint ebo = 0;
    uint32_t indexCount = 0;
};

struct GLBModelGPU {
    std::vector<GLBPrimitiveGPU> primitives;
    std::string modelPath;
    bool valid = false;
};

// Per-instance data (chunk-local position + which chunk)
#pragma pack(push, 1)
struct GLBInstanceData {
    Vec3 localPosition;    // Position within chunk (0-32 range)
    uint32_t chunkDrawID;  // Index into chunk transform SSBO
};
#pragma pack(pop)

// MDI command for each model type
struct GLBDrawCommand {
    uint32_t count;         // Index count
    uint32_t instanceCount; // Number of instances
    uint32_t firstIndex;
    uint32_t baseVertex;
    uint32_t baseInstance; // Offset into instance buffer
};

class GLBModelRenderer {
public:
    GLBModelRenderer();
    ~GLBModelRenderer();

    bool initialize();
    void shutdown();

    // Load a GLB model from disk and upload to GPU
    bool loadModel(uint8_t blockType, const std::string& glbPath);
    
    // Collect instances from all visible chunks and prepare MDI commands
    void updateInstances(const std::vector<VoxelChunk*>& visibleChunks);
    
    // Render to GBuffer using MDI (shares chunk transform SSBO with InstancedQuadRenderer)
    void renderToGBuffer(const glm::mat4& viewProjection, float time);
    
    // Render depth only (shadow pass)
    void renderDepth(const glm::mat4& lightVP, float time);
    
    // Get transform SSBO from InstancedQuadRenderer for shared chunk transforms
    void setChunkTransformSSBO(GLuint ssbo) { m_chunkTransformSSBO = ssbo; }

private:
    // Model registry (blockType -> GPU model)
    std::unordered_map<uint8_t, GLBModelGPU> m_models;
    
    // Unified instance buffer (all models, all chunks)
    GLuint m_instanceSSBO = 0;
    std::vector<GLBInstanceData> m_instances;
    
    // MDI command buffer (per primitive of each model type)
    GLuint m_commandBuffer = 0;
    std::vector<GLBDrawCommand> m_commands;
    
    // Shared chunk transform SSBO (from InstancedQuadRenderer)
    GLuint m_chunkTransformSSBO = 0;
    
    // Shaders (minimal placeholders for now)
    GLuint m_gbufferShader = 0;
    GLuint m_depthShader = 0;
    
    // Uniform locations
    GLint m_gbuffer_uViewProjection = -1;
    GLint m_gbuffer_uTime = -1;
    GLint m_depth_uLightVP = -1;
    
    // Helper methods
    void createShaders();
    GLuint compileShader(const char* source, GLenum type);
    GLuint linkProgram(GLuint vs, GLuint fs);
};

extern std::unique_ptr<GLBModelRenderer> g_glbModelRenderer;
