// GLBModelRenderer.cpp - MDI rendering for GLB block models
#include "GLBModelRenderer.h"
#include "InstancedQuadRenderer.h"
#include "../World/VoxelChunk.h"
#include "../Profiling/Profiler.h"
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

std::unique_ptr<GLBModelRenderer> g_glbModelRenderer;

// Minimal placeholder shaders (gbuffer outputs only, no lighting)
static const char* GBUFFER_VERTEX_SHADER = R"(
#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// Instance data from SSBO
struct InstanceData {
    vec3 localPosition;
    uint chunkDrawID;
};

layout(std430, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

// Chunk transforms (shared with InstancedQuadRenderer)
layout(std430, binding = 0) readonly buffer ChunkTransforms {
    mat4 transforms[];
};

uniform mat4 uViewProjection;
uniform float uTime;

out vec2 vUV;
out vec3 vNormal;
out vec3 vWorldPos;

void main() {
    InstanceData inst = instances[gl_InstanceID];
    mat4 chunkTransform = transforms[inst.chunkDrawID];
    
    // Local position within chunk + vertex offset
    vec4 localPos = vec4(inst.localPosition + aPosition, 1.0);
    vec4 worldPos = chunkTransform * localPos;
    
    gl_Position = uViewProjection * worldPos;
    vUV = aUV;
    vNormal = mat3(chunkTransform) * aNormal;
    vWorldPos = worldPos.xyz;
}
)";

static const char* GBUFFER_FRAGMENT_SHADER = R"(
#version 460 core

in vec2 vUV;
in vec3 vNormal;
in vec3 vWorldPos;

layout(location = 0) out vec3 gAlbedo;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec3 gPosition;
layout(location = 3) out vec4 gMetadata;

void main() {
    // Placeholder: white albedo, just for testing
    gAlbedo = vec3(0.8);
    gNormal = normalize(vNormal);
    gPosition = vWorldPos;
    gMetadata = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

static const char* DEPTH_VERTEX_SHADER = R"(
#version 460 core

layout(location = 0) in vec3 aPosition;

struct InstanceData {
    vec3 localPosition;
    uint chunkDrawID;
};

layout(std430, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(std430, binding = 0) readonly buffer ChunkTransforms {
    mat4 transforms[];
};

uniform mat4 uLightVP;

void main() {
    InstanceData inst = instances[gl_InstanceID];
    mat4 chunkTransform = transforms[inst.chunkDrawID];
    vec4 worldPos = chunkTransform * vec4(inst.localPosition + aPosition, 1.0);
    gl_Position = uLightVP * worldPos;
}
)";

static const char* DEPTH_FRAGMENT_SHADER = R"(
#version 460 core
void main() {}
)";

GLBModelRenderer::GLBModelRenderer() {}

GLBModelRenderer::~GLBModelRenderer() {
    shutdown();
}

bool GLBModelRenderer::initialize() {
    createShaders();
    
    // Create instance SSBO
    glGenBuffers(1, &m_instanceSSBO);
    
    // Create command buffer
    glGenBuffers(1, &m_commandBuffer);
    
    return m_gbufferShader != 0 && m_depthShader != 0;
}

void GLBModelRenderer::shutdown() {
    // Cleanup models
    for (auto& [blockType, model] : m_models) {
        for (auto& prim : model.primitives) {
            if (prim.vao) glDeleteVertexArrays(1, &prim.vao);
            if (prim.vbo) glDeleteBuffers(1, &prim.vbo);
            if (prim.ebo) glDeleteBuffers(1, &prim.ebo);
        }
    }
    m_models.clear();
    
    if (m_instanceSSBO) glDeleteBuffers(1, &m_instanceSSBO);
    if (m_commandBuffer) glDeleteBuffers(1, &m_commandBuffer);
    if (m_gbufferShader) glDeleteProgram(m_gbufferShader);
    if (m_depthShader) glDeleteProgram(m_depthShader);
}

GLuint GLBModelRenderer::compileShader(const char* source, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::cerr << "GLBModelRenderer shader compile error: " << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint GLBModelRenderer::linkProgram(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        std::cerr << "GLBModelRenderer program link error: " << log << std::endl;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void GLBModelRenderer::createShaders() {
    // GBuffer shader
    GLuint vs = compileShader(GBUFFER_VERTEX_SHADER, GL_VERTEX_SHADER);
    GLuint fs = compileShader(GBUFFER_FRAGMENT_SHADER, GL_FRAGMENT_SHADER);
    if (vs && fs) {
        m_gbufferShader = linkProgram(vs, fs);
        m_gbuffer_uViewProjection = glGetUniformLocation(m_gbufferShader, "uViewProjection");
        m_gbuffer_uTime = glGetUniformLocation(m_gbufferShader, "uTime");
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    
    // Depth shader
    vs = compileShader(DEPTH_VERTEX_SHADER, GL_VERTEX_SHADER);
    fs = compileShader(DEPTH_FRAGMENT_SHADER, GL_FRAGMENT_SHADER);
    if (vs && fs) {
        m_depthShader = linkProgram(vs, fs);
        m_depth_uLightVP = glGetUniformLocation(m_depthShader, "uLightVP");
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
}

bool GLBModelRenderer::loadModel(uint8_t blockType, const std::string& glbPath) {
    // Check if already loaded
    if (m_models.find(blockType) != m_models.end()) {
        return m_models[blockType].valid;
    }
    
    // Load GLB from disk
    GLBModelCPU cpuModel;
    if (!GLBLoader::loadGLB(glbPath, cpuModel)) {
        std::cerr << "Failed to load GLB: " << glbPath << std::endl;
        return false;
    }
    
    // Upload to GPU
    GLBModelGPU gpuModel;
    gpuModel.modelPath = glbPath;
    
    for (const auto& cpuPrim : cpuModel.primitives) {
        GLBPrimitiveGPU gpuPrim;
        
        // Create VAO
        glGenVertexArrays(1, &gpuPrim.vao);
        glBindVertexArray(gpuPrim.vao);
        
        // Upload interleaved vertex data
        glGenBuffers(1, &gpuPrim.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gpuPrim.vbo);
        glBufferData(GL_ARRAY_BUFFER, 
                     cpuPrim.interleaved.size() * sizeof(float),
                     cpuPrim.interleaved.data(), 
                     GL_STATIC_DRAW);
        
        // Setup vertex attributes (pos, normal, uv)
        GLsizei stride = 8 * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
        
        // Upload indices
        glGenBuffers(1, &gpuPrim.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpuPrim.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     cpuPrim.indices.size() * sizeof(uint32_t),
                     cpuPrim.indices.data(),
                     GL_STATIC_DRAW);
        
        gpuPrim.indexCount = static_cast<uint32_t>(cpuPrim.indices.size());
        
        glBindVertexArray(0);
        
        gpuModel.primitives.push_back(gpuPrim);
    }
    
    gpuModel.valid = !gpuModel.primitives.empty();
    m_models[blockType] = gpuModel;
    
    std::cout << "Loaded GLB model: " << glbPath << " (" << gpuModel.primitives.size() << " primitives)" << std::endl;
    return true;
}

void GLBModelRenderer::updateInstances(const std::vector<VoxelChunk*>& visibleChunks) {
    PROFILE_SCOPE("GLBModelRenderer_UpdateInstances");
    
    m_instances.clear();
    m_commands.clear();
    
    // For each model type, collect instances from all chunks
    for (const auto& [blockType, model] : m_models) {
        if (!model.valid) continue;
        
        uint32_t baseInstance = static_cast<uint32_t>(m_instances.size());
        uint32_t instanceCount = 0;
        
        // Collect instances from all visible chunks
        for (VoxelChunk* chunk : visibleChunks) {
            if (!chunk) continue;
            
            const auto& chunkInstances = chunk->getModelInstances(blockType);
            if (chunkInstances.empty()) continue;
            
            // Get chunk's draw ID from InstancedQuadRenderer
            extern std::unique_ptr<class InstancedQuadRenderer> g_instancedQuadRenderer;
            int chunkDrawID = g_instancedQuadRenderer->getChunkDrawID(chunk);
            if (chunkDrawID < 0) continue; // Chunk not registered
            
            // Add instances
            for (const Vec3& localPos : chunkInstances) {
                GLBInstanceData inst;
                inst.localPosition = localPos;
                inst.chunkDrawID = static_cast<uint32_t>(chunkDrawID);
                m_instances.push_back(inst);
                instanceCount++;
            }
        }
        
        // Create MDI command for each primitive of this model
        if (instanceCount > 0) {
            for (const auto& prim : model.primitives) {
                GLBDrawCommand cmd;
                cmd.count = prim.indexCount;
                cmd.instanceCount = instanceCount;
                cmd.firstIndex = 0;
                cmd.baseVertex = 0;
                cmd.baseInstance = baseInstance;
                m_commands.push_back(cmd);
            }
        }
    }
    
    // Upload instance data to GPU
    if (!m_instances.empty()) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_instanceSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     m_instances.size() * sizeof(GLBInstanceData),
                     m_instances.data(),
                     GL_DYNAMIC_DRAW);
    }
    
    // Upload MDI commands
    if (!m_commands.empty()) {
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commandBuffer);
        glBufferData(GL_DRAW_INDIRECT_BUFFER,
                     m_commands.size() * sizeof(GLBDrawCommand),
                     m_commands.data(),
                     GL_DYNAMIC_DRAW);
    }
}

void GLBModelRenderer::renderToGBuffer(const glm::mat4& viewProjection, float time) {
    if (m_instances.empty() || m_commands.empty() || !m_gbufferShader) return;
    
    PROFILE_SCOPE("GLBModelRenderer_GBuffer");
    
    glUseProgram(m_gbufferShader);
    glUniformMatrix4fv(m_gbuffer_uViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    glUniform1f(m_gbuffer_uTime, time);
    
    // Bind shared chunk transform SSBO (binding 0)
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_chunkTransformSSBO);
    
    // Bind instance SSBO (binding 1)
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_instanceSSBO);
    
    // Bind command buffer
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commandBuffer);
    
    // Render each model type using MDI
    size_t cmdOffset = 0;
    for (const auto& [blockType, model] : m_models) {
        if (!model.valid) continue;
        
        for (const auto& prim : model.primitives) {
            if (cmdOffset >= m_commands.size()) break;
            
            glBindVertexArray(prim.vao);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                       (void*)(cmdOffset * sizeof(GLBDrawCommand)),
                                       1, 0);
            cmdOffset++;
        }
    }
    
    glBindVertexArray(0);
}

void GLBModelRenderer::renderDepth(const glm::mat4& lightVP, float /*time*/) {
    if (m_instances.empty() || m_commands.empty() || !m_depthShader) return;
    
    PROFILE_SCOPE("GLBModelRenderer_Depth");
    
    glUseProgram(m_depthShader);
    glUniformMatrix4fv(m_depth_uLightVP, 1, GL_FALSE, glm::value_ptr(lightVP));
    
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_chunkTransformSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_instanceSSBO);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commandBuffer);
    
    size_t cmdOffset = 0;
    for (const auto& [blockType, model] : m_models) {
        if (!model.valid) continue;
        
        for (const auto& prim : model.primitives) {
            if (cmdOffset >= m_commands.size()) break;
            
            glBindVertexArray(prim.vao);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                       (void*)(cmdOffset * sizeof(GLBDrawCommand)),
                                       1, 0);
            cmdOffset++;
        }
    }
    
    glBindVertexArray(0);
}
