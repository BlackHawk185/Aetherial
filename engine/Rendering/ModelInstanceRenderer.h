// ModelInstanceRenderer.h - Instanced rendering for GLB-based models (generic multi-model support)
#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <utility>  // for std::pair
#include <glm/glm.hpp>

#include "../Math/Vec3.h"
#include "../World/VoxelChunk.h"

using GLuint = unsigned int;
using GLsizei = int;

// Minimal vertex/mesh structure for GLB
struct ModelPrimitiveGPU {
    GLuint vbo = 0;      // interleaved position, normal, uv
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

struct ModelGPU {
    std::vector<ModelPrimitiveGPU> primitives;
    bool valid = false;
};

// Per-chunk, per-block-type instance buffer
struct ChunkInstanceBuffer {
    GLuint instanceVBO = 0; // mat3x4 or per-instance vec4 data, kept minimal here
    GLsizei count = 0;
    bool isUploaded = false; // Track if data is already uploaded to GPU
    glm::mat4 modelMatrix;   // Island transform * chunk offset (for shadow pass)
    std::vector<GLuint> vaos; // Per-primitive VAOs that bind this chunk's instance buffer
};

// Hash function for std::pair<VoxelChunk*, uint8_t> (for instance buffer map)
struct ChunkBlockPairHash {
    std::size_t operator()(const std::pair<VoxelChunk*, uint8_t>& pair) const {
        return std::hash<void*>()(pair.first) ^ (std::hash<uint8_t>()(pair.second) << 1);
    }
};

class ModelInstanceRenderer {
public:
    ModelInstanceRenderer();
    ~ModelInstanceRenderer();

    bool initialize();
    void shutdown();

    // Generic model loading (NEW)
    bool loadModel(uint8_t blockID, const std::string& glbPath);

    // Update per-frame (for time-based animations)
    void update(float deltaTime);

    // Lighting data (shared with MDIRenderer)
    void setLightingData(const glm::mat4& lightVP, const glm::vec3& lightDir);
    
    // Mark all models as needing lighting recalculation (called when sun direction changes)
    void markLightingDirty() { m_lightingDirty = true; }
    
    // Recalculate lighting for all models if dirty
    void updateLightingIfNeeded();

    // Shadow pass methods (matches MDIRenderer API)
    void beginDepthPass(const glm::mat4& lightVP, int cascadeIndex = 0);
    void renderDepth();
    void endDepthPass(int screenWidth, int screenHeight);
    
    // G-buffer rendering (deferred rendering - writes geometry data only)
    void renderToGBuffer(const glm::mat4& view, const glm::mat4& proj);

    // Update model matrix without rendering (stores pre-calculated chunk transform)
    void updateModelMatrix(uint8_t blockID, VoxelChunk* chunk, const glm::mat4& chunkTransform);

private:
    bool ensureChunkInstancesUploaded(uint8_t blockID, VoxelChunk* chunk);
    bool ensureShaders();
    bool buildGPUFromModel(uint8_t blockID);
    GLuint compileShaderForBlock(uint8_t blockID);  // NEW: Per-model shader compilation

    // Internal GL helpers
    // Per-model shaders (wind vs static)
    std::unordered_map<uint8_t, GLuint> m_shaders;
    
    // G-buffer shaders (deferred rendering)
    std::unordered_map<uint8_t, GLuint> m_gbufferShaders;
    
    // Depth pass shader (for shadow map rendering)
    GLuint m_depthProgram = 0;
    int m_depth_uLightVP = -1;
    int m_depth_uModel = -1;
    int m_depth_uTime = -1;  // Wind animation in shadow pass

    // Shadow/lighting (shared with MDIRenderer)
    glm::mat4 m_lightVP;
    glm::vec3 m_lightDir{ -0.3f, -1.0f, -0.2f };

    // Time for animations
    float m_time = 0.0f;

    // NEW: Multiple GPU models by BlockID
    std::unordered_map<uint8_t, ModelGPU> m_models;
    std::unordered_map<uint8_t, std::string> m_modelPaths;  // Track loaded paths
    std::unordered_map<uint8_t, GLuint> m_albedoTextures;   // Per-model textures
    GLuint m_engineGrassTex = 0;  // Engine grass.png texture (for grass model)
    
    // NEW: CPU-side model data for lighting recalculation
    std::unordered_map<uint8_t, struct GLBModelCPU> m_cpuModels;
    bool m_lightingDirty = true;  // Track when sun direction changes

    // NEW: Instance buffers per (chunk, blockID) pair
    std::unordered_map<std::pair<VoxelChunk*, uint8_t>, ChunkInstanceBuffer, ChunkBlockPairHash> m_chunkInstances;
};

// Global instance (owned pointer)
extern std::unique_ptr<ModelInstanceRenderer> g_modelRenderer;
