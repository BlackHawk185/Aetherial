#include "ModelInstanceRenderer.h"
#include "../Assets/GLBLoader.h"
#include "CascadedShadowMap.h"
#include <glad/gl.h>
#include "TextureManager.h"
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <cstdio>
#include <tiny_gltf.h>

// Global instance
std::unique_ptr<ModelInstanceRenderer> g_modelRenderer = nullptr;
extern ShadowMap g_shadowMap;

namespace {
    // ========== DEPTH SHADERS (for shadow map rendering) ==========
    static const char* kDepthVS = R"GLSL(
#version 460 core
layout (location=0) in vec3 aPos;
layout (location=4) in vec4 aInstance; // xyz=position offset, w=phase

uniform mat4 uModel;       // chunk/world offset
uniform mat4 uLightVP;
uniform float uTime;

void main(){
    // Apply same wind animation as forward shader for correct shadow positioning
    float windStrength = 0.15;
    float heightFactor = max(0.0, aPos.y * 0.8);
    vec3 windOffset = vec3(
        sin(uTime * 1.8 + aInstance.w * 2.0) * windStrength * heightFactor,
        0.0,
        cos(uTime * 1.4 + aInstance.w * 1.7) * windStrength * heightFactor * 0.7
    );
    
    vec4 world = uModel * vec4(aPos + windOffset + aInstance.xyz, 1.0);
    gl_Position = uLightVP * world;
}
)GLSL";

    static const char* kDepthFS = R"GLSL(
#version 460 core
void main(){
    // Depth is written automatically to depth buffer
}
)GLSL";

    // ========== FORWARD SHADERS (for main rendering) ==========
    // Wind-animated shader for grass/foliage
    static const char* kVS_Wind = R"GLSL(
#version 460 core
layout (location=0) in vec3 aPos;
layout (location=1) in vec3 aNormal;
layout (location=2) in vec2 aUV;
layout (location=4) in vec4 aInstance; // xyz=position offset (voxel center), w=phase

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;       // chunk/world offset
uniform mat4 uLightVP;
uniform float uTime;

out vec2 vUV;   
out vec3 vNormalWS;
out vec3 vWorldPos;
out vec4 vLightSpacePos;
out float vViewZ;

void main(){
    // Wind sway: affect vertices based on their height within the grass model
    // Higher vertices (larger Y) sway more, creating natural grass movement
    float windStrength = 0.15;
    float heightFactor = max(0.0, aPos.y * 0.8); // Scale with vertex height
    vec3 windOffset = vec3(
        sin(uTime * 1.8 + aInstance.w * 2.0) * windStrength * heightFactor,
        0.0,
        cos(uTime * 1.4 + aInstance.w * 1.7) * windStrength * heightFactor * 0.7
    );
    
    vec4 world = uModel * vec4(aPos + windOffset + aInstance.xyz, 1.0);
    gl_Position = uProjection * uView * world;
    vUV = aUV;
    vNormalWS = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    vWorldPos = world.xyz;
    vLightSpacePos = uLightVP * world;
    vViewZ = -(uView * world).z;
}
)GLSL";
    
    // Static shader for non-animated models (QFG, rocks, etc.)
    static const char* kVS_Static = R"GLSL(
#version 460 core
layout (location=0) in vec3 aPos;
layout (location=1) in vec3 aNormal;
layout (location=2) in vec2 aUV;
layout (location=4) in vec4 aInstance; // xyz=position offset, w=unused

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;       // chunk/world offset
uniform mat4 uLightVP;
uniform float uTime;

out vec2 vUV;   
out vec3 vNormalWS;
out vec3 vWorldPos;
out vec4 vLightSpacePos;
out float vViewZ;

void main(){
    // No wind animation - static model
    vec4 world = uModel * vec4(aPos + aInstance.xyz, 1.0);
    gl_Position = uProjection * uView * world;
    vUV = aUV;
    vNormalWS = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    vWorldPos = world.xyz;
    vLightSpacePos = uLightVP * world;
    vViewZ = -(uView * world).z;
}
)GLSL";

    static const char* kFS = R"GLSL(
#version 460 core
in vec2 vUV;
in vec3 vNormalWS;
in vec3 vWorldPos;
in vec4 vLightSpacePos;
in float vViewZ;

uniform sampler2DArrayShadow uShadowMap;  // Cascaded shadow map array
uniform float uShadowTexel;
uniform vec3 uLightDir;
uniform sampler2D uGrassTexture; // engine grass texture with alpha

// Cascade uniforms
uniform mat4 uCascadeVP[2];      // View-projection for each cascade
uniform float uCascadeSplits[2];  // Split distances for cascades
uniform int uNumCascades;         // Number of cascades (typically 2)

out vec4 FragColor;

// Poisson disk with 32 samples for high-quality soft shadows (match voxel shader)
const vec2 POISSON[32] = vec2[32](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790),
    vec2(-0.52748980, -0.18467720), vec2(0.64042155, 0.55584620),
    vec2(-0.58689597, 0.67128760), vec2(0.24767240, -0.51805620),
    vec2(-0.09192791, -0.54150760), vec2(0.89877152, -0.24330990),
    vec2(0.33697340, 0.90091330), vec2(-0.41818693, -0.85628360),
    vec2(0.69197035, -0.06798679), vec2(-0.97010720, 0.16373110),
    vec2(0.06372385, 0.37408390), vec2(-0.63902735, -0.56419730),
    vec2(0.56546623, 0.25234550), vec2(-0.23892370, 0.51662970),
    vec2(0.13814290, 0.98162460), vec2(-0.46671060, 0.16780830)
);

float sampleShadowPCF(float bias)
{
    // Hard cascade switch with overlap zone
    // Near cascade: 0-128 blocks (high detail)
    // Far cascade: 32+ blocks (low detail, large coverage)
    // Overlap zone: 32-128 blocks (both render, near wins)
    float viewDepth = abs(vViewZ);
    
    // Use near cascade if within 128 blocks
    int cascadeIndex = (viewDepth <= 128.0) ? 0 : 1;
    
    // Transform to light space for selected cascade
    vec4 lightSpacePos = uCascadeVP[cascadeIndex] * vec4(vWorldPos, 1.0);
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    proj = proj * 0.5 + 0.5;
    
    // If outside light frustum, surface receives NO light (dark by default)
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z > 1.0)
        return 0.0;
    
    float current = proj.z - bias;
    
    // Adjust PCF radius based on cascade to maintain consistent world-space blur
    float baseRadius = 512.0;
    float radiusScale = (cascadeIndex == 0) ? 1.0 : 0.125;  // 1/8 for far cascade
    float radius = baseRadius * radiusScale * uShadowTexel;
    
    // Sample center first using array shadow sampler
    float center = texture(uShadowMap, vec4(proj.xy, cascadeIndex, current));
    
    // Early exit if fully lit - prevents shadow bleeding
    if (center >= 1.0) {
        return 1.0;
    }
    
    // Poisson disk sampling
    float sum = center;
    for (int i = 0; i < 32; ++i) {
        vec2 offset = POISSON[i] * radius;
        float d = texture(uShadowMap, vec4(proj.xy + offset, cascadeIndex, current));
        sum += d;
    }
    
    // Average and lighten-only
    return max(center, sum / 33.0);  // 33 = 32 samples + 1 center
}

void main(){
    // Slope-scale bias based on surface angle to light
    vec3 N = normalize(vNormalWS);
    vec3 L = normalize(-uLightDir);
    float ndotl = max(dot(N, L), 0.0);
    float bias = max(0.0, 0.0001 * (1.0 - ndotl));
    
    float visibility = sampleShadowPCF(bias);

    vec4 albedo = texture(uGrassTexture, vUV);
    // Alpha cutout
    if (albedo.a < 0.3) discard;
    
    // Apply PCF shadow visibility (no ambient, no lambert - pure shadow map)
    // Visibility = 1.0 means fully lit, 0.0 means fully shadowed
    vec3 lit = albedo.rgb * visibility;
    FragColor = vec4(lit, 1.0);
}
)GLSL";

    // ========== G-BUFFER SHADERS (for deferred rendering) ==========
    static const char* kGBuffer_FS = R"GLSL(
#version 460 core
in vec2 vUV;
in vec3 vNormalWS;
in vec3 vWorldPos;
in vec4 vLightSpacePos;
in float vViewZ;

uniform sampler2D uGrassTexture;

// G-buffer outputs (MRT)
layout(location = 0) out vec3 gAlbedo;    // Base color
layout(location = 1) out vec3 gNormal;    // World-space normal
layout(location = 2) out vec3 gPosition;  // World position
layout(location = 3) out vec4 gMetadata;  // Reserved for future use

void main(){
    vec4 albedo = texture(uGrassTexture, vUV);
    
    // Alpha cutout
    if (albedo.a < 0.3) discard;
    
    // Write to G-buffer
    gAlbedo = albedo.rgb;
    gNormal = normalize(vNormalWS);
    gPosition = vWorldPos;
    gMetadata = vec4(0.0);  // Reserved (could store material properties later)
}
)GLSL";

}

static GLuint Compile(GLuint type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log);
        std::cerr << "Shader compile error: " << log << std::endl;
        glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint Link(GLuint vs, GLuint fs = 0) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    if (fs) glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(p, 1024, nullptr, log);
        std::cerr << "Program link error: " << log << std::endl;
        glDeleteProgram(p); return 0;
    }
    return p;
}

ModelInstanceRenderer::ModelInstanceRenderer() {}
ModelInstanceRenderer::~ModelInstanceRenderer() { shutdown(); }

// Compile shader for specific block type
GLuint ModelInstanceRenderer::compileShaderForBlock(uint8_t blockID) {
    // Determine which vertex shader to use based on block type
    const char* vertexShader = kVS_Static;  // Default to static (no wind)
    
    // Wind animation for grass and foliage
    if (blockID == 13) {  // BlockID::DECOR_GRASS
        vertexShader = kVS_Wind;
    }
    // TODO: Add other wind-animated blocks (leaves, reeds, etc.)
    
    // Compile and link
    GLuint vs = Compile(GL_VERTEX_SHADER, vertexShader);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, kFS);  // Same fragment shader for all
    if (!vs || !fs) return 0;
    
    GLuint program = Link(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    return program;
}

bool ModelInstanceRenderer::initialize() {
    // Shaders are now compiled lazily per-block type
    return true;
}

void ModelInstanceRenderer::shutdown() {
    // Clean up all instance buffers and their VAOs
    for (auto& kv : m_chunkInstances) {
        if (kv.second.instanceVBO) glDeleteBuffers(1, &kv.second.instanceVBO);
        if (!kv.second.vaos.empty()) {
            glDeleteVertexArrays(static_cast<GLsizei>(kv.second.vaos.size()), kv.second.vaos.data());
        }
    }
    m_chunkInstances.clear();
    
    // Clean up all loaded models
    for (auto& modelPair : m_models) {
        for (auto& prim : modelPair.second.primitives) {
            if (prim.vbo) glDeleteBuffers(1, &prim.vbo);
            if (prim.ebo) glDeleteBuffers(1, &prim.ebo);
        }
    }
    m_models.clear();
    
    // Clean up textures
    for (auto& texPair : m_albedoTextures) {
        if (texPair.second) glDeleteTextures(1, &texPair.second);
    }
    m_albedoTextures.clear();
    if (m_engineGrassTex) { glDeleteTextures(1, &m_engineGrassTex); m_engineGrassTex = 0; }
    
    // Clean up per-block shaders
    for (auto& shaderPair : m_shaders) {
        if (shaderPair.second) glDeleteProgram(shaderPair.second);
    }
    m_shaders.clear();
    
    // Clean up G-buffer shaders
    for (auto& shaderPair : m_gbufferShaders) {
        if (shaderPair.second) glDeleteProgram(shaderPair.second);
    }
    m_gbufferShaders.clear();
    
    // Clean up depth shader
    if (m_depthProgram) { glDeleteProgram(m_depthProgram); m_depthProgram = 0; }
}

bool ModelInstanceRenderer::loadModel(uint8_t blockID, const std::string& path) {
    // Check if already loaded
    if (m_models.find(blockID) != m_models.end() && m_modelPaths[blockID] == path) {
        return m_models[blockID].valid;
    }
    
    // Load GLB file - try multiple path candidates
    GLBModelCPU cpu;
    std::vector<std::string> candidates{ 
        path,
        std::string("../") + path,
        std::string("../../") + path,
        std::string("../../../") + path,
        std::string("C:/Users/steve-17/Desktop/game2/") + path
    };
    
    bool ok = false;
    std::string resolvedPath;
    
    // Try each path without spamming errors - GLBLoader will only log internally
    for (auto& p : candidates) {
        // Check if file exists before attempting load to avoid error spam
        if (std::filesystem::exists(p)) {
            if (GLBLoader::loadGLB(p, cpu)) {
                ok = true;
                resolvedPath = p;
                break;
            }
        }
    }
    
    if (!ok) {
        // Only log once after all attempts failed
        std::cerr << "Warning: Failed to load model from '" << path << "'" << std::endl;
        return false;
    }

    // Clear any existing model for this blockID
    auto it = m_models.find(blockID);
    if (it != m_models.end()) {
        for (auto& prim : it->second.primitives) {
            if (prim.vbo) glDeleteBuffers(1, &prim.vbo);
            if (prim.ebo) glDeleteBuffers(1, &prim.ebo);
        }
    }
    
    // Build GPU model from CPU data (VBO/EBO only - VAOs created per-chunk)
    ModelGPU gpuModel;
    gpuModel.primitives.clear();
    for (auto& cpuPrim : cpu.primitives) {
        ModelPrimitiveGPU gp;
        
        // Create and upload vertex buffer (initially with default lighting)
        glGenBuffers(1, &gp.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gp.vbo);
        glBufferData(GL_ARRAY_BUFFER, cpuPrim.interleaved.size() * sizeof(float), cpuPrim.interleaved.data(), GL_DYNAMIC_DRAW);  // DYNAMIC for lighting updates
        
        // Create and upload index buffer
        if (!cpuPrim.indices.empty()) {
            glGenBuffers(1, &gp.ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gp.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, cpuPrim.indices.size() * sizeof(unsigned int), cpuPrim.indices.data(), GL_STATIC_DRAW);
        }
        
        gp.indexCount = (int)cpuPrim.indices.size();
        gpuModel.primitives.emplace_back(gp);
    }
    gpuModel.valid = !gpuModel.primitives.empty();
    
    // Store both CPU and GPU models
    m_cpuModels[blockID] = std::move(cpu);  // Keep CPU data for lighting recalculation
    m_models[blockID] = gpuModel;
    m_modelPaths[blockID] = path;

    // Load base color texture from GLB (first material's baseColorTexture)
    GLuint albedoTex = 0;
    try {
        tinygltf::TinyGLTF gltf;
        tinygltf::Model model;
        std::string err, warn;
        if (gltf.LoadBinaryFromFile(&model, &err, &warn, resolvedPath)) {
            int texIndex = -1;
            if (!model.materials.empty()) {
                const auto& mat = model.materials[0];
                if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
                    texIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
                }
            }
            if (texIndex >= 0 && texIndex < (int)model.textures.size()) {
                int imgIndex = model.textures[texIndex].source;
                if (imgIndex >= 0 && imgIndex < (int)model.images.size()) {
                    const auto& img = model.images[imgIndex];
                    GLenum fmt = (img.component == 4) ? GL_RGBA : (img.component == 3) ? GL_RGB : GL_RED;
                    glGenTextures(1, &albedoTex);
                    glBindTexture(GL_TEXTURE_2D, albedoTex);
                    glTexImage2D(GL_TEXTURE_2D, 0, fmt, img.width, img.height, 0, fmt, GL_UNSIGNED_BYTE, img.image.data());
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                    glGenerateMipmap(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
            }
        }
    } catch (...) {
        // ignore texture loading failures
    }
    m_albedoTextures[blockID] = albedoTex;
    
    // Special case: Load engine grass.png texture for grass block (BlockID::DECOR_GRASS = 13)
    if (blockID == 13) {
        if (!g_textureManager) g_textureManager = new TextureManager();
        m_engineGrassTex = g_textureManager->getTexture("grass.png");
        if (m_engineGrassTex == 0) {
            const char* candidates[] = {
                "assets/textures/",
                "../assets/textures/",
                "../../assets/textures/",
                "../../../assets/textures/"
            };
            for (const auto& dir : candidates) {
                std::filesystem::path p = std::filesystem::path(dir) / "grass.png";
                if (std::filesystem::exists(p)) { 
                    m_engineGrassTex = g_textureManager->loadTexture(p.string(), false, true); 
                    break;
                }
            }
            if (m_engineGrassTex == 0) {
                std::filesystem::path fallback("C:/Users/steve-17/Desktop/game2/assets/textures/grass.png");
                if (std::filesystem::exists(fallback)) {
                    m_engineGrassTex = g_textureManager->loadTexture(fallback.string(), false, true);
                }
            }
        }
    }

    // NEW: Compile shader for this block type if not already compiled
    if (m_shaders.find(blockID) == m_shaders.end()) {
        GLuint shader = compileShaderForBlock(blockID);
        if (shader == 0) {
            std::cerr << "Failed to compile shader for block " << (int)blockID << std::endl;
            return false;
        }
        m_shaders[blockID] = shader;
    }

    return gpuModel.valid;
}

void ModelInstanceRenderer::update(float deltaTime) {
    m_time += deltaTime;
}

void ModelInstanceRenderer::setLightingData(const glm::mat4& lightVP, const glm::vec3& lightDir) {
    // Check if sun direction changed
    glm::vec3 prevDir = m_lightDir;
    m_lightVP = lightVP;
    m_lightDir = lightDir;
    
    // Mark lighting dirty if sun direction changed significantly
    float dotDiff = glm::dot(prevDir, lightDir);
    if (dotDiff < 0.9999f) {  // Threshold for change detection
        m_lightingDirty = true;
    }
}

void ModelInstanceRenderer::updateLightingIfNeeded() {
    // No longer need to update vertex buffers - lighting is calculated in shader
    m_lightingDirty = false;
}


bool ModelInstanceRenderer::ensureChunkInstancesUploaded(uint8_t blockID, VoxelChunk* chunk) {
    if (!chunk) return false;
    
    // Check if model is loaded
    auto modelIt = m_models.find(blockID);
    if (modelIt == m_models.end() || !modelIt->second.valid) return false;
    
    // Get instances for this block type
    const auto& instances = chunk->getModelInstances(blockID);
    GLsizei count = static_cast<GLsizei>(instances.size());
    if (count == 0) return false;

    // Buffer must already exist (created by updateModelMatrix)
    auto key = std::make_pair(chunk, blockID);
    auto it = m_chunkInstances.find(key);
    if (it == m_chunkInstances.end()) return false; // Should never happen
    
    ChunkInstanceBuffer& buf = it->second;
    
    // Create per-chunk VAOs if they don't exist (one VAO per primitive)
    if (buf.vaos.empty()) {
        buf.vaos.resize(modelIt->second.primitives.size());
        for (size_t i = 0; i < modelIt->second.primitives.size(); ++i) {
            const auto& prim = modelIt->second.primitives[i];
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            
            // Bind the model's vertex/index buffers
            glBindBuffer(GL_ARRAY_BUFFER, prim.vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prim.ebo);
            
            // Setup vertex attributes: pos(3), normal(3), uv(2) = 8 floats
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float)*8, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float)*8, (void*)(sizeof(float)*3));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(float)*8, (void*)(sizeof(float)*6));
            
            // Bind this chunk's instance buffer (location 4)
            glBindBuffer(GL_ARRAY_BUFFER, buf.instanceVBO);
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(float)*4, (void*)0);
            glVertexAttribDivisor(4, 1);
            
            glBindVertexArray(0);
            buf.vaos[i] = vao;
        }
    }
    
    // Check if we need to update (mesh may be null during async generation)
    auto mesh = chunk->getRenderMesh();
    bool needsUpdate = !mesh;
    
    // Only upload if data hasn't been uploaded yet or chunk mesh needs update
    if (buf.isUploaded && !needsUpdate && buf.count == count) {
        return true; // Already up to date
    }
    
    // Build per-instance data vec4(x,y,z,phase). Phase derived from position for determinism
    std::vector<float> data;
    data.resize(count * 4);
    for (size_t i=0;i<instances.size();i++) {
        data[i*4+0] = instances[i].x;
        data[i*4+1] = instances[i].y;
        data[i*4+2] = instances[i].z;
        // Simple hash-based phase
        float phase = fmodf((instances[i].x*12.9898f + instances[i].z*78.233f) * 43758.5453f, 6.28318f);
        data[i*4+3] = phase;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buf.instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, data.size()*sizeof(float), data.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    buf.count = count;
    buf.isUploaded = true;

    return true;
}

void ModelInstanceRenderer::updateModelMatrix(uint8_t blockID, VoxelChunk* chunk, const glm::mat4& chunkTransform) {
    // Store the pre-calculated chunk transform FIRST (before uploading instances)
    auto key = std::make_pair(chunk, blockID);
    auto bufIt = m_chunkInstances.find(key);
    if (bufIt != m_chunkInstances.end()) {
        bufIt->second.modelMatrix = chunkTransform;
    } else {
        // Buffer doesn't exist yet - create it with the correct matrix
        ChunkInstanceBuffer buf;
        glGenBuffers(1, &buf.instanceVBO);
        buf.modelMatrix = chunkTransform;
        m_chunkInstances.emplace(key, buf);
    }
    
    // NOW upload instances with the correct matrix already set
    ensureChunkInstancesUploaded(blockID, chunk);
}

void ModelInstanceRenderer::renderAll(const glm::mat4& view, const glm::mat4& proj) {
    // Update lighting once for all models
    updateLightingIfNeeded();
    
    // Set global GL state once
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    glReadBuffer(GL_BACK);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    
    // Disable culling once for foliage rendering
    GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
    if (wasCull) glDisable(GL_CULL_FACE);
    
    // Extract camera position from view matrix for distance culling
    glm::mat4 invView = glm::inverse(view);
    glm::vec3 cameraPos = glm::vec3(invView[3]);
    const float maxRenderDistance = 512.0f;  // LOD render limit for GLB objects
    const float maxRenderDistanceSq = maxRenderDistance * maxRenderDistance;
    
    // Calculate shadow map data once
    int numCascades = g_shadowMap.getNumCascades();
    int shadowSize = g_shadowMap.getSize();
    float shadowTexel = shadowSize > 0 ? (1.0f / float(shadowSize)) : (1.0f / 8192.0f);
    
    // Bind shadow map texture once (all shaders use same binding)
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_shadowMap.getDepthTexture());
    
    // Iterate through each block type with loaded models
    for (const auto& [blockID, model] : m_models) {
        if (!model.valid) continue;
        
        // Get shader for this block type
        auto shaderIt = m_shaders.find(blockID);
        if (shaderIt == m_shaders.end()) continue;
        GLuint shader = shaderIt->second;
        
        // Bind shader ONCE per block type
        glUseProgram(shader);
        
        // Cache uniform locations for this shader
        int loc_View = glGetUniformLocation(shader, "uView");
        int loc_Proj = glGetUniformLocation(shader, "uProjection");
        int loc_Model = glGetUniformLocation(shader, "uModel");
        int loc_Time = glGetUniformLocation(shader, "uTime");
        int loc_LightDir = glGetUniformLocation(shader, "uLightDir");
        int loc_ShadowTexel = glGetUniformLocation(shader, "uShadowTexel");
        int loc_ShadowMap = glGetUniformLocation(shader, "uShadowMap");
        int loc_Texture = glGetUniformLocation(shader, "uGrassTexture");
        int loc_NumCascades = glGetUniformLocation(shader, "uNumCascades");
        
        // Set uniforms that are constant across all chunks (ONCE per block type)
        glUniformMatrix4fv(loc_View, 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(loc_Proj, 1, GL_FALSE, &proj[0][0]);
        glUniform1f(loc_Time, m_time);
        glUniform3f(loc_LightDir, m_lightDir.x, m_lightDir.y, m_lightDir.z);
        glUniform1f(loc_ShadowTexel, shadowTexel);
        glUniform1i(loc_ShadowMap, 7);
        glUniform1i(loc_NumCascades, numCascades);
        
        // Set cascade shadow map data ONCE per block type
        for (int i = 0; i < numCascades; ++i) {
            const CascadeData& cascade = g_shadowMap.getCascade(i);
            std::string vpName = "uCascadeVP[" + std::to_string(i) + "]";
            std::string splitName = "uCascadeSplits[" + std::to_string(i) + "]";
            int loc_CascadeVP = glGetUniformLocation(shader, vpName.c_str());
            int loc_CascadeSplit = glGetUniformLocation(shader, splitName.c_str());
            glUniformMatrix4fv(loc_CascadeVP, 1, GL_FALSE, &cascade.viewProj[0][0]);
            glUniform1f(loc_CascadeSplit, cascade.splitDistance);
        }
        
        // Bind texture ONCE per block type
        if (loc_Texture >= 0) {
            glActiveTexture(GL_TEXTURE5);
            GLuint tex = 0;
            if (blockID == 13 && m_engineGrassTex) {  // 13 = BlockID::DECOR_GRASS
                tex = m_engineGrassTex;
            } else {
                auto texIt = m_albedoTextures.find(blockID);
                if (texIt != m_albedoTextures.end()) {
                    tex = texIt->second;
                }
            }
            if (tex) {
                glBindTexture(GL_TEXTURE_2D, tex);
                glUniform1i(loc_Texture, 5);
            }
        }
        
        // Now render ALL chunks that have instances of this block type
        for (auto& [key, buf] : m_chunkInstances) {
            // Only render chunks with this specific blockID
            if (key.second != blockID) continue;
            if (buf.count == 0) continue;
            if (!buf.isUploaded) continue;
            
            // Distance culling: Extract chunk position from model matrix and check distance
            glm::vec3 chunkPos = glm::vec3(buf.modelMatrix[3]);
            glm::vec3 delta = cameraPos - chunkPos;
            float distanceSq = glm::dot(delta, delta);
            if (distanceSq > maxRenderDistanceSq) continue;  // Skip distant chunks
            
            // Set model matrix (this is the ONLY per-chunk uniform)
            glUniformMatrix4fv(loc_Model, 1, GL_FALSE, &buf.modelMatrix[0][0]);
            
            // Render instanced models using per-chunk VAOs
            for (size_t i = 0; i < buf.vaos.size() && i < model.primitives.size(); ++i) {
                glBindVertexArray(buf.vaos[i]);
                glDrawElementsInstanced(GL_TRIANGLES, model.primitives[i].indexCount, GL_UNSIGNED_INT, 0, buf.count);
            }
        }
    }
    
    // Cleanup
    glBindVertexArray(0);
    if (wasCull) glEnable(GL_CULL_FACE);
}

void ModelInstanceRenderer::renderToGBuffer(const glm::mat4& view, const glm::mat4& proj) {
    // Disable culling for foliage rendering
    GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
    if (wasCull) glDisable(GL_CULL_FACE);
    
    // Extract camera position for distance culling
    glm::mat4 invView = glm::inverse(view);
    glm::vec3 cameraPos = glm::vec3(invView[3]);
    const float maxRenderDistance = 512.0f;
    const float maxRenderDistanceSq = maxRenderDistance * maxRenderDistance;
    
    // Iterate through each block type
    for (const auto& [blockID, model] : m_models) {
        if (!model.valid) continue;
        
        // Get or compile G-buffer shader for this block type
        GLuint shader = 0;
        auto shaderIt = m_gbufferShaders.find(blockID);
        if (shaderIt == m_gbufferShaders.end()) {
            // Compile G-buffer shader (uses same vertex shader, different fragment shader)
            const char* vs = (blockID == 13) ? kVS_Wind : kVS_Static;
            GLuint vsShader = Compile(GL_VERTEX_SHADER, vs);
            GLuint fsShader = Compile(GL_FRAGMENT_SHADER, kGBuffer_FS);
            if (vsShader && fsShader) {
                shader = Link(vsShader, fsShader);
                glDeleteShader(vsShader);
                glDeleteShader(fsShader);
                m_gbufferShaders[blockID] = shader;
            } else {
                continue;
            }
        } else {
            shader = shaderIt->second;
        }
        
        if (!shader) continue;
        
        // Bind shader
        glUseProgram(shader);
        
        // Set uniforms
        int loc_View = glGetUniformLocation(shader, "uView");
        int loc_Proj = glGetUniformLocation(shader, "uProjection");
        int loc_Model = glGetUniformLocation(shader, "uModel");
        int loc_Time = glGetUniformLocation(shader, "uTime");
        int loc_Texture = glGetUniformLocation(shader, "uGrassTexture");
        
        glUniformMatrix4fv(loc_View, 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(loc_Proj, 1, GL_FALSE, &proj[0][0]);
        glUniform1f(loc_Time, m_time);
        
        // Bind texture
        if (loc_Texture >= 0) {
            glActiveTexture(GL_TEXTURE5);
            GLuint tex = 0;
            if (blockID == 13 && m_engineGrassTex) {
                tex = m_engineGrassTex;
            } else {
                auto texIt = m_albedoTextures.find(blockID);
                if (texIt != m_albedoTextures.end()) {
                    tex = texIt->second;
                }
            }
            if (tex) {
                glBindTexture(GL_TEXTURE_2D, tex);
                glUniform1i(loc_Texture, 5);
            }
        }
        
        // Render all chunks with this block type
        for (auto& [key, buf] : m_chunkInstances) {
            if (key.second != blockID) continue;
            if (buf.count == 0 || !buf.isUploaded) continue;
            
            // Distance culling
            glm::vec3 chunkPos = glm::vec3(buf.modelMatrix[3]);
            glm::vec3 delta = cameraPos - chunkPos;
            float distanceSq = glm::dot(delta, delta);
            if (distanceSq > maxRenderDistanceSq) continue;
            
            // Set model matrix
            glUniformMatrix4fv(loc_Model, 1, GL_FALSE, &buf.modelMatrix[0][0]);
            
            // Render instances
            for (size_t i = 0; i < buf.vaos.size() && i < model.primitives.size(); ++i) {
                glBindVertexArray(buf.vaos[i]);
                glDrawElementsInstanced(GL_TRIANGLES, model.primitives[i].indexCount, GL_UNSIGNED_INT, 0, buf.count);
            }
        }
    }
    
    // Cleanup
    glBindVertexArray(0);
    if (wasCull) glEnable(GL_CULL_FACE);
}

// ========== SHADOW PASS METHODS ==========

void ModelInstanceRenderer::beginDepthPass(const glm::mat4& lightVP, int cascadeIndex)
{
    // Compile depth shader if not already done
    if (m_depthProgram == 0) {
        GLuint vs = Compile(GL_VERTEX_SHADER, kDepthVS);
        GLuint fs = Compile(GL_FRAGMENT_SHADER, kDepthFS);
        if (vs && fs) {
            m_depthProgram = Link(vs, fs);
            glDeleteShader(vs);
            glDeleteShader(fs);
            
            if (m_depthProgram) {
                m_depth_uLightVP = glGetUniformLocation(m_depthProgram, "uLightVP");
                m_depth_uModel = glGetUniformLocation(m_depthProgram, "uModel");
                m_depth_uTime = glGetUniformLocation(m_depthProgram, "uTime");
            }
        }
    }
    
    if (m_depthProgram == 0) return;  // Failed to compile
    
    // Shadow map begin() already called by GameClient - just set shader uniforms
    
    glUseProgram(m_depthProgram);
    if (m_depth_uLightVP != -1) {
        glUniformMatrix4fv(m_depth_uLightVP, 1, GL_FALSE, &lightVP[0][0]);
    }
    if (m_depth_uTime != -1) {
        glUniform1f(m_depth_uTime, m_time);  // Apply wind animation to shadows
    }
}

void ModelInstanceRenderer::renderDepth()
{
    if (m_depthProgram == 0) return;  // Not initialized
    
    // Render all uploaded instances into shadow map
    for (auto& [key, buf] : m_chunkInstances) {
        auto [chunk, blockID] = key;
        
        if (buf.count == 0) continue;
        
        // Find model GPU data
        auto modelIt = m_models.find(blockID);
        if (modelIt == m_models.end()) continue;
        
        // Use stored model matrix from forward pass
        glm::mat4 model = buf.modelMatrix;
        
        if (m_depth_uModel != -1) {
            glUniformMatrix4fv(m_depth_uModel, 1, GL_FALSE, &model[0][0]);
        }
        
        // Culling already disabled by CascadedShadowMap::begin() - don't touch it
        
        // Render instanced models using per-chunk VAOs
        for (size_t i = 0; i < buf.vaos.size() && i < modelIt->second.primitives.size(); ++i) {
            glBindVertexArray(buf.vaos[i]);
            glDrawElementsInstanced(GL_TRIANGLES, modelIt->second.primitives[i].indexCount, GL_UNSIGNED_INT, 0, buf.count);
            glBindVertexArray(0);
        }
    }
}

void ModelInstanceRenderer::endDepthPass(int screenWidth, int screenHeight)
{
    // Shadow map end() will be called by GameClient after all depth rendering
    // This method exists for API consistency but doesn't need to do anything
    (void)screenWidth;
    (void)screenHeight;
}

