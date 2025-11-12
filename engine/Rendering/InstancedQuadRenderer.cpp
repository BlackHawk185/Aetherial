// InstancedQuadRenderer.cpp - GPU instanced rendering implementation
#include "InstancedQuadRenderer.h"
#include "../World/VoxelChunk.h"
#include "../Math/Vec3.h"
#include "TextureManager.h"
#include "CascadedShadowMap.h"
#include "../Time/DayNightController.h"
#include "../Profiling/Profiler.h"

#include <iostream>
#include <glm/gtc/type_ptr.hpp>

// MDI command structure
struct DrawElementsIndirectCommand {
    uint32_t count;          // 6 (indices per quad)
    uint32_t instanceCount;  // number of quads in this chunk
    uint32_t firstIndex;     // always 0 (shared EBO)
    uint32_t baseVertex;     // always 0
    uint32_t baseInstance;   // offset into instance data buffer
};

// Global instance
std::unique_ptr<InstancedQuadRenderer> g_instancedQuadRenderer;

// External globals
extern ShadowMap g_shadowMap;
extern DayNightController* g_dayNightController;

// ============================================================================
// DEFERRED RENDERING SHADERS (G-Buffer Pass)
// ============================================================================
// These shaders write geometry data to G-buffer. Lighting is applied later
// by DeferredLightingPass which samples shadows once for the entire screen.
// ============================================================================

static const char* DEPTH_VERTEX_SHADER_MDI = R"(
#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aInstancePosition;
layout(location = 2) in vec3 aInstanceNormal;
layout(location = 3) in float aInstanceWidth;
layout(location = 4) in float aInstanceHeight;
layout(location = 5) in uint aInstanceBlockType;
layout(location = 6) in uint aInstanceFaceDir;

uniform mat4 uLightVP;

layout(std430, binding = 0) readonly buffer ChunkTransforms {
    mat4 transforms[];
};

void main() {
    mat4 uChunkTransform = transforms[gl_DrawID];
    
    vec3 scaledPos = vec3(
        aPosition.x * aInstanceWidth,
        aPosition.y * aInstanceHeight,
        0.0
    );
    
    vec3 rotatedPos;
    if (aInstanceFaceDir == 0u) {
        rotatedPos = vec3(scaledPos.x, 0.0, scaledPos.y);
    } else if (aInstanceFaceDir == 1u) {
        rotatedPos = vec3(-scaledPos.x, 0.0, scaledPos.y);
    } else if (aInstanceFaceDir == 2u) {
        rotatedPos = vec3(-scaledPos.x, scaledPos.y, 0.0);
    } else if (aInstanceFaceDir == 3u) {
        rotatedPos = vec3(scaledPos.x, scaledPos.y, 0.0);
    } else if (aInstanceFaceDir == 4u) {
        rotatedPos = vec3(0.0, scaledPos.y, scaledPos.x);
    } else {
        rotatedPos = vec3(0.0, scaledPos.y, -scaledPos.x);
    }
    
    vec3 localPos = aInstancePosition + rotatedPos;
    vec4 worldPos = uChunkTransform * vec4(localPos, 1.0);
    gl_Position = uLightVP * worldPos;
}
)";

static const char* DEPTH_FRAGMENT_SHADER = R"(
#version 460 core
void main() {}
)";

static const char* GBUFFER_VERTEX_SHADER_MDI = R"(
#version 460 core

// Unit quad vertex attributes
layout(location = 0) in vec3 aPosition;

// Instance attributes
layout(location = 1) in vec3 aInstancePosition;
layout(location = 2) in vec3 aInstanceNormal;
layout(location = 3) in float aInstanceWidth;
layout(location = 4) in float aInstanceHeight;
layout(location = 5) in uint aInstanceBlockType;
layout(location = 6) in uint aInstanceFaceDir;

uniform mat4 uViewProjection;

// SSBO for chunk transforms (indexed by gl_DrawID)
layout(std430, binding = 0) readonly buffer ChunkTransforms {
    mat4 transforms[];
};

out vec2 TexCoord;
out vec3 Normal;
out vec3 WorldPos;
flat out uint BlockType;
flat out uint FaceDir;

void main() {
    mat4 uChunkTransform = transforms[gl_DrawID];
    
    // Same vertex transformation as forward pass
    vec3 scaledPos = vec3(
        aPosition.x * aInstanceWidth,
        aPosition.y * aInstanceHeight,
        0.0
    );
    
    vec3 rotatedPos;
    if (aInstanceFaceDir == 0u) {
        rotatedPos = vec3(scaledPos.x, 0.0, scaledPos.y);
    } else if (aInstanceFaceDir == 1u) {
        rotatedPos = vec3(-scaledPos.x, 0.0, scaledPos.y);
    } else if (aInstanceFaceDir == 2u) {
        rotatedPos = vec3(-scaledPos.x, scaledPos.y, 0.0);
    } else if (aInstanceFaceDir == 3u) {
        rotatedPos = vec3(scaledPos.x, scaledPos.y, 0.0);
    } else if (aInstanceFaceDir == 4u) {
        rotatedPos = vec3(0.0, scaledPos.y, scaledPos.x);
    } else {
        rotatedPos = vec3(0.0, scaledPos.y, -scaledPos.x);
    }
    
    vec3 localPos = aInstancePosition + rotatedPos;
    vec4 worldPos4 = uChunkTransform * vec4(localPos, 1.0);
    WorldPos = worldPos4.xyz;
    gl_Position = uViewProjection * worldPos4;
    
    TexCoord = (aPosition.xy + 0.5) * vec2(aInstanceWidth, aInstanceHeight);
    Normal = mat3(uChunkTransform) * aInstanceNormal;
    BlockType = aInstanceBlockType;
    FaceDir = aInstanceFaceDir;
}
)";

static const char* GBUFFER_FRAGMENT_SHADER = R"(
#version 460 core

in vec2 TexCoord;
in vec3 Normal;
in vec3 WorldPos;
flat in uint BlockType;
flat in uint FaceDir;

uniform sampler2DArray uBlockTextures;  // Texture array with all block textures

// G-buffer outputs (MRT)
layout(location = 0) out vec3 gAlbedo;    // Base color
layout(location = 1) out vec3 gNormal;    // World-space normal
layout(location = 2) out vec3 gPosition;  // World position
layout(location = 3) out vec4 gMetadata;  // BlockType (R), FaceDir (G)

void main() {
    // Sample texture from array using BlockType as layer index
    vec4 texColor = texture(uBlockTextures, vec3(TexCoord, float(BlockType)));
    
    // Write to G-buffer
    gAlbedo = texColor.rgb;
    gNormal = normalize(Normal);
    gPosition = WorldPos;
    gMetadata = vec4(float(BlockType) / 255.0, float(FaceDir) / 255.0, 0.0, 1.0);
}
)";

// GPU Frustum Culling Compute Shader
static const char* FRUSTUM_CULL_COMPUTE = R"(
#version 460 core
layout(local_size_x = 64) in;

struct ChunkAABB {
    vec3 minBounds;
    float pad1;
    vec3 maxBounds;
    float pad2;
};

layout(std430, binding = 0) readonly buffer ChunkBounds {
    ChunkAABB chunks[];
};

layout(std430, binding = 1) writeonly buffer Visibility {
    uint visible[];
};

uniform mat4 uViewProjection;

// Frustum planes extracted from view-projection matrix
vec4 frustumPlanes[6];

void extractFrustumPlanes() {
    mat4 vp = uViewProjection;
    
    frustumPlanes[0] = vec4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]); // Left
    frustumPlanes[1] = vec4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]); // Right
    frustumPlanes[2] = vec4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]); // Bottom
    frustumPlanes[3] = vec4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]); // Top
    frustumPlanes[4] = vec4(vp[0][3] + vp[0][2], vp[1][3] + vp[1][2], vp[2][3] + vp[2][2], vp[3][3] + vp[3][2]); // Near
    frustumPlanes[5] = vec4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]); // Far
    
    for (int i = 0; i < 6; i++) {
        float len = length(frustumPlanes[i].xyz);
        frustumPlanes[i] /= len;
    }
}

bool testAABB(vec3 minBounds, vec3 maxBounds) {
    for (int i = 0; i < 6; i++) {
        vec4 plane = frustumPlanes[i];
        vec3 positiveVertex = vec3(
            plane.x > 0.0 ? maxBounds.x : minBounds.x,
            plane.y > 0.0 ? maxBounds.y : minBounds.y,
            plane.z > 0.0 ? maxBounds.z : minBounds.z
        );
        
        float dist = dot(plane.xyz, positiveVertex) + plane.w;
        if (dist < 0.0) return false;
    }
    return true;
}

void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= chunks.length()) return;
    
    extractFrustumPlanes();
    
    ChunkAABB chunk = chunks[index];
    visible[index] = testAABB(chunk.minBounds, chunk.maxBounds) ? 1u : 0u;
}
)";

InstancedQuadRenderer::InstancedQuadRenderer()
    : m_unitQuadVAO(0), m_unitQuadVBO(0), m_unitQuadEBO(0), m_gbufferMDIProgram(0), m_depthMDIProgram(0),
      m_transformSSBO(0), m_mdiCommandBuffer(0), m_mdiInstanceBuffer(0), m_mdiVAO(0),
      m_persistentQuadBuffer(0), m_frustumCullProgram(0), m_visibilitySSBO(0)
{
}

InstancedQuadRenderer::~InstancedQuadRenderer()
{
    shutdown();
}

bool InstancedQuadRenderer::initialize()
{
    createUnitQuad();
    createGBufferShader();  // For deferred rendering
    createDepthShader();    // For shadow casting
    
    // Initialize and load block textures directly into texture array
    extern TextureManager* g_textureManager;
    if (!g_textureManager) {
        g_textureManager = new TextureManager();
    }
    
    // Always ensure it's initialized (safe to call multiple times)
    if (!g_textureManager->initialize()) {
        std::cerr << "❌ Failed to initialize texture manager" << std::endl;
        return false;
    }
    
    if (!loadBlockTextureArray()) {
        std::cerr << "❌ Failed to load block texture array" << std::endl;
        return false;
    }
    
    glGenBuffers(1, &m_transformSSBO);
    glGenBuffers(1, &m_mdiCommandBuffer);
    glGenBuffers(1, &m_mdiInstanceBuffer);
    glGenVertexArrays(1, &m_mdiVAO);
    
    // Persistent mapped buffer for quad data (GL 4.4+) - unified for all chunks
    m_persistentQuadCapacity = 1024 * 1024 * 64; // 64MB for all chunks (512M quads)
    m_persistentQuadUsed = 0;
    glGenBuffers(1, &m_persistentQuadBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_persistentQuadBuffer);
    glBufferStorage(GL_ARRAY_BUFFER, m_persistentQuadCapacity * sizeof(QuadFace), nullptr, 
                    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_DYNAMIC_STORAGE_BIT);
    m_persistentQuadPtr = glMapBufferRange(GL_ARRAY_BUFFER, 0, m_persistentQuadCapacity * sizeof(QuadFace),
                                           GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    
    // Persistent mapped buffer for draw commands
    size_t maxChunks = 4096;  // Support up to 4096 chunks
    glGenBuffers(1, &m_persistentCommandBuffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_persistentCommandBuffer);
    glBufferStorage(GL_DRAW_INDIRECT_BUFFER, maxChunks * sizeof(DrawElementsIndirectCommand), nullptr,
                    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_DYNAMIC_STORAGE_BIT);
    m_persistentCommandPtr = glMapBufferRange(GL_DRAW_INDIRECT_BUFFER, 0, maxChunks * sizeof(DrawElementsIndirectCommand),
                                              GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    
    // Persistent mapped buffer for transforms
    glGenBuffers(1, &m_persistentTransformBuffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_persistentTransformBuffer);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, maxChunks * sizeof(glm::mat4), nullptr,
                    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_DYNAMIC_STORAGE_BIT);
    m_persistentTransformPtr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, maxChunks * sizeof(glm::mat4),
                                                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    
    // GPU frustum culling
    glGenBuffers(1, &m_visibilitySSBO);
    createFrustumCullShader();
    
    // Configure MDI VAO (shared for all chunks)
    glBindVertexArray(m_mdiVAO);
    
    // Bind unit quad vertices (attribute 0)
    glBindBuffer(GL_ARRAY_BUFFER, m_unitQuadVBO);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    
    // Bind element buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_unitQuadEBO);
    
    // Instance attributes bound to persistent quad buffer (all chunks in one buffer)
    glBindBuffer(GL_ARRAY_BUFFER, m_persistentQuadBuffer);
    
    size_t offset = 0;
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset); glVertexAttribDivisor(1, 1); offset += sizeof(Vec3);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset); glVertexAttribDivisor(2, 1); offset += sizeof(Vec3);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset); glVertexAttribDivisor(3, 1); offset += sizeof(float);
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset); glVertexAttribDivisor(4, 1); offset += sizeof(float);
    glEnableVertexAttribArray(5); glVertexAttribIPointer(5, 1, GL_UNSIGNED_BYTE, sizeof(QuadFace), (void*)offset); glVertexAttribDivisor(5, 1); offset += sizeof(uint8_t);
    glEnableVertexAttribArray(6); glVertexAttribIPointer(6, 1, GL_UNSIGNED_BYTE, sizeof(QuadFace), (void*)offset); glVertexAttribDivisor(6, 1);
    
    glBindVertexArray(0);
    
    return true;
}

void InstancedQuadRenderer::createUnitQuad()
{
    // Unit quad vertices: corners at ±0.5 in XY plane
    float quadVertices[] = {
        -0.5f, -0.5f, 0.0f,  // Bottom-left
         0.5f, -0.5f, 0.0f,  // Bottom-right
         0.5f,  0.5f, 0.0f,  // Top-right
        -0.5f,  0.5f, 0.0f   // Top-left
    };
    
    // Two triangles forming a quad (counter-clockwise winding when viewed from +Z)
    uint32_t quadIndices[] = {
        0, 1, 2,  // First triangle
        0, 2, 3   // Second triangle
    };
    
    // Create VAO/VBO/EBO
    glGenVertexArrays(1, &m_unitQuadVAO);
    glGenBuffers(1, &m_unitQuadVBO);
    glGenBuffers(1, &m_unitQuadEBO);
    
    glBindVertexArray(m_unitQuadVAO);
    
    // Upload vertex data
    glBindBuffer(GL_ARRAY_BUFFER, m_unitQuadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    
    // Upload index data
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_unitQuadEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quadIndices), quadIndices, GL_STATIC_DRAW);
    
    // Vertex position attribute (location 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    
    glBindVertexArray(0);
}

void InstancedQuadRenderer::createDepthShader()
{
    GLuint vertexShaderMDI = compileShader(DEPTH_VERTEX_SHADER_MDI, GL_VERTEX_SHADER);
    GLuint fragmentShaderMDI = compileShader(DEPTH_FRAGMENT_SHADER, GL_FRAGMENT_SHADER);
    
    m_depthMDIProgram = glCreateProgram();
    glAttachShader(m_depthMDIProgram, vertexShaderMDI);
    glAttachShader(m_depthMDIProgram, fragmentShaderMDI);
    glLinkProgram(m_depthMDIProgram);
    
    GLint success;
    glGetProgramiv(m_depthMDIProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_depthMDIProgram, 512, nullptr, infoLog);
        std::cerr << "❌ Depth MDI shader linking FAILED:\n" << infoLog << std::endl;
    }
    
    glDeleteShader(vertexShaderMDI);
    glDeleteShader(fragmentShaderMDI);
    
    m_depthMDI_uLightVP = glGetUniformLocation(m_depthMDIProgram, "uLightVP");
}

void InstancedQuadRenderer::createGBufferShader()
{
    GLuint vertexShaderMDI = compileShader(GBUFFER_VERTEX_SHADER_MDI, GL_VERTEX_SHADER);
    GLuint fragmentShaderMDI = compileShader(GBUFFER_FRAGMENT_SHADER, GL_FRAGMENT_SHADER);
    
    m_gbufferMDIProgram = glCreateProgram();
    glAttachShader(m_gbufferMDIProgram, vertexShaderMDI);
    glAttachShader(m_gbufferMDIProgram, fragmentShaderMDI);
    glLinkProgram(m_gbufferMDIProgram);
    
    GLint success;
    glGetProgramiv(m_gbufferMDIProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_gbufferMDIProgram, 512, nullptr, infoLog);
        std::cerr << "❌ G-buffer MDI shader linking FAILED:\n" << infoLog << std::endl;
    }
    
    glDeleteShader(vertexShaderMDI);
    glDeleteShader(fragmentShaderMDI);
    
    m_gbufferMDI_uViewProjection = glGetUniformLocation(m_gbufferMDIProgram, "uViewProjection");
    m_gbufferMDI_uBlockTextures = glGetUniformLocation(m_gbufferMDIProgram, "uBlockTextures");
}

bool InstancedQuadRenderer::loadBlockTextureArray()
{
    extern TextureManager* g_textureManager;
    
    // Define all 46 block textures in ID order (matching BlockID enum)
    const char* blockTextureFiles[46] = {
        "stone.png",          // 0  - AIR (fallback)
        "stone.png",          // 1  - STONE
        "dirt.png",           // 2  - DIRT
        "gravel.png",         // 3  - GRAVEL
        "clay.png",           // 4  - CLAY
        "moss.png",           // 5  - MOSS
        "sand.png",           // 6  - SAND
        "wood_oak.png",       // 7  - WOOD_OAK
        "wood_birch.png",     // 8  - WOOD_BIRCH
        "wood_pine.png",      // 9  - WOOD_PINE
        "wood_jungle.png",    // 10 - WOOD_JUNGLE
        "wood_palm.png",      // 11 - WOOD_PALM
        "leaves_green.png",   // 12 - LEAVES_GREEN
        "leaves_dark.png",    // 13 - LEAVES_DARK
        "leaves_palm.png",    // 14 - LEAVES_PALM
        "ice.png",            // 15 - ICE
        "packed_ice.png",     // 16 - PACKED_ICE
        "snow.png",           // 17 - SNOW
        "sandstone.png",      // 18 - SANDSTONE
        "granite.png",        // 19 - GRANITE
        "basalt.png",         // 20 - BASALT
        "limestone.png",      // 21 - LIMESTONE
        "marble.png",         // 22 - MARBLE
        "obsidian.png",       // 23 - OBSIDIAN
        "lava_rock.png",      // 24 - LAVA_ROCK
        "volcanic_ash.png",   // 25 - VOLCANIC_ASH
        "magma.png",          // 26 - MAGMA
        "lava.png",           // 27 - LAVA
        "coal.png",           // 28 - COAL
        "iron_block.png",     // 29 - IRON_BLOCK
        "copper_block.png",   // 30 - COPPER_BLOCK
        "gold_block.png",     // 31 - GOLD_BLOCK
        "diamond_block.png",  // 32 - DIAMOND_BLOCK
        "emerald_block.png",  // 33 - EMERALD_BLOCK
        "ruby_block.png",     // 34 - RUBY_BLOCK
        "sapphire_block.png", // 35 - SAPPHIRE_BLOCK
        "amethyst.png",       // 36 - AMETHYST
        "quartz.png",         // 37 - QUARTZ
        "crystal_blue.png",   // 38 - CRYSTAL_BLUE
        "crystal_green.png",  // 39 - CRYSTAL_GREEN
        "crystal_purple.png", // 40 - CRYSTAL_PURPLE
        "crystal_pink.png",   // 41 - CRYSTAL_PINK
        "salt_block.png",     // 42 - SALT_BLOCK
        "mushroom_block.png", // 43 - MUSHROOM_BLOCK
        "coral.png",          // 44 - CORAL
        "water.png"           // 45 - WATER
    };
    
    // Create 2D texture array (46 layers, 32x32 RGBA, no mipmaps)
    glGenTextures(1, &m_blockTextureArray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_blockTextureArray);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 32, 32, 46);
    
    int successCount = 0;
    
    for (int i = 0; i < 46; ++i)
    {
        TextureData texData = g_textureManager->loadTextureData(blockTextureFiles[i]);
        
        if (texData.isValid())
        {
            // Resize if needed (textures should be 32x32)
            if (texData.width == 32 && texData.height == 32)
            {
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, 32, 32, 1, GL_RGBA, GL_UNSIGNED_BYTE, texData.pixels);
                successCount++;
            }
        }
    }
    
    // Set texture parameters (pixel art style - nearest neighbor)
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    
    return successCount > 0;
}

GLuint InstancedQuadRenderer::compileShader(const char* source, GLenum type)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        const char* typeStr = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "❌ " << typeStr << " shader compilation FAILED:\n" << infoLog << std::endl;
    }
    
    return shader;
}

void InstancedQuadRenderer::registerChunk(VoxelChunk* chunk, const glm::mat4& transform)
{
    auto it = m_chunkToIndex.find(chunk);
    if (it != m_chunkToIndex.end())
    {
        m_chunks[it->second].transform = transform;
        return;
    }
    
    size_t index = m_chunks.size();
    
    ChunkEntry entry;
    entry.chunk = chunk;
    entry.transform = transform;
    entry.instanceCount = 0;
    entry.vbo = 0;
    entry.lastUploadedCount = 0;
    entry.chunkID = static_cast<uint32_t>(index);
    
    m_chunks.push_back(entry);
    m_chunkToIndex[chunk] = index;
}

void InstancedQuadRenderer::registerChunkWithSize(VoxelChunk* chunk, const glm::mat4& transform, size_t estimatedQuads)
{
    (void)estimatedQuads;
    
    auto it = m_chunkToIndex.find(chunk);
    if (it != m_chunkToIndex.end())
    {
        m_chunks[it->second].transform = transform;
        return;
    }
    
    size_t index = m_chunks.size();
    
    ChunkEntry entry;
    entry.chunk = chunk;
    entry.transform = transform;
    entry.instanceCount = 0;
    entry.vbo = 0;
    entry.chunkID = static_cast<uint32_t>(index);
    
    m_chunks.push_back(entry);
    m_chunkToIndex[chunk] = index;
}

void InstancedQuadRenderer::uploadChunkInstances(ChunkEntry& entry)
{
    if (!entry.chunk)
    {
        entry.instanceCount = 0;
        return;
    }
    
    // Get existing mesh - don't generate synchronously (async system handles it)
    auto mesh = entry.chunk->getRenderMesh();
    if (!mesh)
    {
        entry.instanceCount = 0;
        return;
    }
    
    entry.instanceCount = mesh->quads.size();
}

void InstancedQuadRenderer::updateChunkTransform(VoxelChunk* chunk, const glm::mat4& transform)
{
    auto it = m_chunkToIndex.find(chunk);
    if (it != m_chunkToIndex.end())
    {
        m_chunks[it->second].transform = transform;
    }
}

void InstancedQuadRenderer::uploadChunkMesh(VoxelChunk* chunk)
{
    auto it = m_chunkToIndex.find(chunk);
    if (it != m_chunkToIndex.end())
    {
        updateSingleChunkGPU(m_chunks[it->second]);
    }
}


// Helper: Calculate padded slot allocation for chunk growth
static size_t calculateChunkSlots(size_t activeQuads)
{
    if (activeQuads == 0) return 256;  // Minimum allocation
    
    // Add 25% padding, round up to nearest 256
    size_t withPadding = activeQuads + (activeQuads / 4);
    return ((withPadding + 255) / 256) * 256;
}

// Update single chunk GPU data
void InstancedQuadRenderer::updateSingleChunkGPU(ChunkEntry& entry)
{
    auto mesh = entry.chunk->getRenderMesh();
    if (!mesh) {
        entry.instanceCount = 0;
        return;  // No mesh yet - workers still processing
    }
    
    size_t newQuadCount = mesh->quads.size();
    
    // Upload if count changed OR if upload was explicitly requested
    if (newQuadCount != entry.lastUploadedCount || mesh->needsGPUUpload) {
        // Check if we need to allocate or reallocate space
        if (entry.allocatedSlots == 0) {
            // FIRST TIME ALLOCATION: Assign baseInstance offset
            entry.allocatedSlots = calculateChunkSlots(newQuadCount);
            entry.baseInstance = m_persistentQuadUsed;
            m_persistentQuadUsed += entry.allocatedSlots;
        } 
        else if (newQuadCount > entry.allocatedSlots) {
            // REALLOCATION NEEDED: Chunk grew beyond padding
            // TODO: Implement defragmentation or just expand in place if possible
            std::cout << "[GPU] WARNING: Chunk grew beyond allocated slots, needs rebuild" << std::endl;
            entry.allocatedSlots = calculateChunkSlots(newQuadCount);
            // Keep existing baseInstance - will overrun, but at least visible
        }
        
        // Write data to persistent buffer at chunk's baseInstance offset
        if (newQuadCount > 0 && m_persistentQuadPtr && entry.baseInstance < m_persistentQuadCapacity) {
            QuadFace* dest = static_cast<QuadFace*>(m_persistentQuadPtr) + entry.baseInstance;
            memcpy(dest, mesh->quads.data(), newQuadCount * sizeof(QuadFace));
            
            // Flush the written range
            glBindBuffer(GL_ARRAY_BUFFER, m_persistentQuadBuffer);
            glFlushMappedBufferRange(GL_ARRAY_BUFFER,
                                     entry.baseInstance * sizeof(QuadFace),
                                     newQuadCount * sizeof(QuadFace));
        }
        mesh->needsGPUUpload = false;
    }
    
    entry.instanceCount = newQuadCount;
    entry.lastUploadedCount = newQuadCount;
}

void InstancedQuadRenderer::renderToGBufferMDI(const glm::mat4& viewProjection, const glm::mat4& view)
{
    (void)view;
    
    PROFILE_SCOPE("QuadRenderer_GBuffer_MDI");
    
    if (m_chunks.empty() || !m_persistentCommandPtr || !m_persistentTransformPtr) return;
    
    // Write draw commands and transforms directly to persistent buffers
    DrawElementsIndirectCommand* cmdPtr = static_cast<DrawElementsIndirectCommand*>(m_persistentCommandPtr);
    glm::mat4* transformPtr = static_cast<glm::mat4*>(m_persistentTransformPtr);
    
    size_t drawCount = 0;
    for (const auto& entry : m_chunks) {
        if (entry.instanceCount == 0) continue;
        
        cmdPtr[drawCount].count = 6;
        cmdPtr[drawCount].instanceCount = entry.instanceCount;
        cmdPtr[drawCount].firstIndex = 0;
        cmdPtr[drawCount].baseVertex = 0;  // Always 0 for unit quad
        cmdPtr[drawCount].baseInstance = entry.baseInstance;  // Offset into instance buffer
        
        transformPtr[drawCount] = entry.transform;
        drawCount++;
    }
    
    if (drawCount == 0) return;
    
    // Flush persistent buffers
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_persistentCommandBuffer);
    glFlushMappedBufferRange(GL_DRAW_INDIRECT_BUFFER, 0, drawCount * sizeof(DrawElementsIndirectCommand));
    
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_persistentTransformBuffer);
    glFlushMappedBufferRange(GL_SHADER_STORAGE_BUFFER, 0, drawCount * sizeof(glm::mat4));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_persistentTransformBuffer);
    
    // Use MDI shader
    glUseProgram(m_gbufferMDIProgram);
    glUniformMatrix4fv(m_gbufferMDI_uViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    
    // Bind texture array
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_blockTextureArray);
    glUniform1i(m_gbufferMDI_uBlockTextures, 0);
    
    // Bind MDI VAO and render
    glBindVertexArray(m_mdiVAO);
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, drawCount, 0);
    glBindVertexArray(0);
}

void InstancedQuadRenderer::renderToGBufferCulledMDI(const glm::mat4& viewProjection, const glm::mat4& view, const std::vector<VoxelChunk*>& visibleChunks)
{
    (void)view;
    
    PROFILE_SCOPE("QuadRenderer_GBuffer_MDI_Culled");
    
    if (visibleChunks.empty() || !m_persistentCommandPtr || !m_persistentTransformPtr) return;
    
    // Write draw commands and transforms directly to persistent buffers for visible chunks only
    DrawElementsIndirectCommand* cmdPtr = static_cast<DrawElementsIndirectCommand*>(m_persistentCommandPtr);
    glm::mat4* transformPtr = static_cast<glm::mat4*>(m_persistentTransformPtr);
    
    size_t drawCount = 0;
    for (VoxelChunk* visChunk : visibleChunks) {
        auto it = m_chunkToIndex.find(visChunk);
        if (it == m_chunkToIndex.end()) continue;
        
        const auto& entry = m_chunks[it->second];
        if (entry.instanceCount == 0) continue;
        
        cmdPtr[drawCount].count = 6;
        cmdPtr[drawCount].instanceCount = entry.instanceCount;
        cmdPtr[drawCount].firstIndex = 0;
        cmdPtr[drawCount].baseVertex = 0;  // Always 0 for unit quad
        cmdPtr[drawCount].baseInstance = entry.baseInstance;  // Offset into instance buffer
        
        transformPtr[drawCount] = entry.transform;
        drawCount++;
    }
    
    if (drawCount == 0) return;
    
    // Flush persistent buffers
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_persistentCommandBuffer);
    glFlushMappedBufferRange(GL_DRAW_INDIRECT_BUFFER, 0, drawCount * sizeof(DrawElementsIndirectCommand));
    
    glFlushMappedBufferRange(GL_SHADER_STORAGE_BUFFER, 0, drawCount * sizeof(glm::mat4));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_persistentTransformBuffer);
    
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    // Use MDI shader
    glUseProgram(m_gbufferMDIProgram);
    glUniformMatrix4fv(m_gbufferMDI_uViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_blockTextureArray);
    glUniform1i(m_gbufferMDI_uBlockTextures, 0);
    
    glBindVertexArray(m_mdiVAO);
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, drawCount, 0);
    glBindVertexArray(0);
}

void InstancedQuadRenderer::renderToGBufferCulledMDI_GPU(const glm::mat4& viewProjection, const glm::mat4& view)
{
    (void)view;
    
    PROFILE_SCOPE("QuadRenderer_GBuffer_MDI_GPU_Culled");
    
    std::vector<VoxelChunk*> visibleChunks;
    cullChunksGPU(viewProjection, visibleChunks);
    
    renderToGBufferCulledMDI(viewProjection, view, visibleChunks);
}

void InstancedQuadRenderer::clear()
{
    for (auto& entry : m_chunks) {
        if (entry.vbo != 0) {
            freeVBO(entry.vbo);
        }
    }
    
    m_chunks.clear();
    m_chunkToIndex.clear();
}

void InstancedQuadRenderer::shutdown()
{
    clear();
    
    if (m_unitQuadVAO != 0) {
        glDeleteVertexArrays(1, &m_unitQuadVAO);
        m_unitQuadVAO = 0;
    }
    
    if (m_unitQuadVBO != 0) {
        glDeleteBuffers(1, &m_unitQuadVBO);
        m_unitQuadVBO = 0;
    }
    
    if (m_unitQuadEBO != 0) {
        glDeleteBuffers(1, &m_unitQuadEBO);
        m_unitQuadEBO = 0;
    }
    
    // Delete MDI buffers
    for (auto& chunk : m_chunks) {
        if (chunk.vbo != 0) {
            glDeleteBuffers(1, &chunk.vbo);
        }
    }
    
    if (m_transformSSBO != 0) {
        glDeleteBuffers(1, &m_transformSSBO);
        m_transformSSBO = 0;
    }
    
    if (m_gbufferMDIProgram != 0) {
        glDeleteProgram(m_gbufferMDIProgram);
        m_gbufferMDIProgram = 0;
    }
    
    if (m_depthMDIProgram != 0) {
        glDeleteProgram(m_depthMDIProgram);
        m_depthMDIProgram = 0;
    }
    
    if (m_persistentQuadBuffer != 0) {
        if (m_persistentQuadPtr) {
            glBindBuffer(GL_ARRAY_BUFFER, m_persistentQuadBuffer);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            m_persistentQuadPtr = nullptr;
        }
        glDeleteBuffers(1, &m_persistentQuadBuffer);
        m_persistentQuadBuffer = 0;
    }
    
    if (m_persistentCommandBuffer != 0) {
        if (m_persistentCommandPtr) {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_persistentCommandBuffer);
            glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
            m_persistentCommandPtr = nullptr;
        }
        glDeleteBuffers(1, &m_persistentCommandBuffer);
        m_persistentCommandBuffer = 0;
    }
    
    if (m_persistentTransformBuffer != 0) {
        if (m_persistentTransformPtr) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_persistentTransformBuffer);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            m_persistentTransformPtr = nullptr;
        }
        glDeleteBuffers(1, &m_persistentTransformBuffer);
        m_persistentTransformBuffer = 0;
    }
    
    if (m_frustumCullProgram != 0) {
        glDeleteProgram(m_frustumCullProgram);
        m_frustumCullProgram = 0;
    }
    
    if (m_visibilitySSBO != 0) {
        glDeleteBuffers(1, &m_visibilitySSBO);
        m_visibilitySSBO = 0;
    }
    
    for (GLuint vbo : m_freeVBOPool) {
        glDeleteBuffers(1, &vbo);
    }
    m_freeVBOPool.clear();
}

// ========== SHADOW DEPTH PASS METHODS ==========

void InstancedQuadRenderer::beginDepthPass(const glm::mat4& lightVP, int /*cascadeIndex*/)
{
    glUseProgram(m_depthMDIProgram);
    glUniformMatrix4fv(m_depthMDI_uLightVP, 1, GL_FALSE, glm::value_ptr(lightVP));
}

void InstancedQuadRenderer::renderDepthMDI()
{
    PROFILE_SCOPE("QuadRenderer_Depth_MDI");
    
    if (m_chunks.empty() || !m_persistentCommandPtr || !m_persistentTransformPtr) return;
    
    // Write draw commands and transforms directly to persistent buffers
    DrawElementsIndirectCommand* cmdPtr = static_cast<DrawElementsIndirectCommand*>(m_persistentCommandPtr);
    glm::mat4* transformPtr = static_cast<glm::mat4*>(m_persistentTransformPtr);
    
    size_t drawCount = 0;
    for (const auto& entry : m_chunks) {
        if (entry.instanceCount == 0) continue;
        
        cmdPtr[drawCount].count = 6;
        cmdPtr[drawCount].instanceCount = entry.instanceCount;
        cmdPtr[drawCount].firstIndex = 0;
        cmdPtr[drawCount].baseVertex = 0;  // Always 0 for unit quad
        cmdPtr[drawCount].baseInstance = entry.baseInstance;  // Offset into instance buffer
        
        transformPtr[drawCount] = entry.transform;
        drawCount++;
    }
    
    if (drawCount == 0) return;
    
    // Flush persistent buffers
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_persistentCommandBuffer);
    glFlushMappedBufferRange(GL_DRAW_INDIRECT_BUFFER, 0, drawCount * sizeof(DrawElementsIndirectCommand));
    
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_persistentTransformBuffer);
    glFlushMappedBufferRange(GL_SHADER_STORAGE_BUFFER, 0, drawCount * sizeof(glm::mat4));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_persistentTransformBuffer);
    
    // Render
    glBindVertexArray(m_mdiVAO);
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, drawCount, 0);
    glBindVertexArray(0);
}

void InstancedQuadRenderer::renderDepthCulledMDI(const std::vector<VoxelChunk*>& visibleChunks)
{
    PROFILE_SCOPE("QuadRenderer_Depth_MDI_Culled");
    
    if (visibleChunks.empty() || !m_persistentCommandPtr || !m_persistentTransformPtr) return;
    
    // Write draw commands and transforms directly to persistent buffers for visible chunks only
    DrawElementsIndirectCommand* cmdPtr = static_cast<DrawElementsIndirectCommand*>(m_persistentCommandPtr);
    glm::mat4* transformPtr = static_cast<glm::mat4*>(m_persistentTransformPtr);
    
    size_t drawCount = 0;
    for (VoxelChunk* visChunk : visibleChunks) {
        auto it = m_chunkToIndex.find(visChunk);
        if (it == m_chunkToIndex.end()) continue;
        
        const auto& entry = m_chunks[it->second];
        if (entry.instanceCount == 0) continue;
        
        cmdPtr[drawCount].count = 6;
        cmdPtr[drawCount].instanceCount = entry.instanceCount;
        cmdPtr[drawCount].firstIndex = 0;
        cmdPtr[drawCount].baseVertex = 0;  // Always 0 for unit quad
        cmdPtr[drawCount].baseInstance = entry.baseInstance;  // Offset into instance buffer
        
        transformPtr[drawCount] = entry.transform;
        drawCount++;
    }
    
    if (drawCount == 0) return;
    
    // Flush persistent buffers
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_persistentCommandBuffer);
    glFlushMappedBufferRange(GL_DRAW_INDIRECT_BUFFER, 0, drawCount * sizeof(DrawElementsIndirectCommand));
    
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_persistentTransformBuffer);
    glFlushMappedBufferRange(GL_SHADER_STORAGE_BUFFER, 0, drawCount * sizeof(glm::mat4));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_persistentTransformBuffer);
    
    // Render
    glBindVertexArray(m_mdiVAO);
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, drawCount, 0);
    glBindVertexArray(0);
}

void InstancedQuadRenderer::renderDepthCulledMDI_GPU(const glm::mat4& viewProjection)
{
    PROFILE_SCOPE("QuadRenderer_Depth_MDI_GPU_Culled");
    
    std::vector<VoxelChunk*> visibleChunks;
    cullChunksGPU(viewProjection, visibleChunks);
    
    renderDepthCulledMDI(visibleChunks);
}

void InstancedQuadRenderer::endDepthPass(int screenWidth, int screenHeight)
{
    (void)screenWidth;
    (void)screenHeight;
}

void InstancedQuadRenderer::createFrustumCullShader()
{
    GLuint compute = compileShader(FRUSTUM_CULL_COMPUTE, GL_COMPUTE_SHADER);
    m_frustumCullProgram = glCreateProgram();
    glAttachShader(m_frustumCullProgram, compute);
    glLinkProgram(m_frustumCullProgram);
    
    GLint success;
    glGetProgramiv(m_frustumCullProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(m_frustumCullProgram, 512, nullptr, log);
        std::cerr << "Frustum cull shader link failed: " << log << std::endl;
    }
    
    glDeleteShader(compute);
}

void InstancedQuadRenderer::cullChunksGPU(const glm::mat4& viewProj, std::vector<VoxelChunk*>& outVisible)
{
    if (m_chunks.empty()) return;
    
    struct ChunkAABB {
        glm::vec3 minBounds;
        float pad1;
        glm::vec3 maxBounds;
        float pad2;
    };
    
    std::vector<ChunkAABB> aabbs;
    aabbs.reserve(m_chunks.size());
    
    for (const auto& entry : m_chunks) {
        const auto& cachedAABB = entry.chunk->getCachedWorldAABB();
        ChunkAABB aabb;
        aabb.minBounds = glm::vec3(cachedAABB.min.x, cachedAABB.min.y, cachedAABB.min.z);
        aabb.maxBounds = glm::vec3(cachedAABB.max.x, cachedAABB.max.y, cachedAABB.max.z);
        aabbs.push_back(aabb);
    }
    
    GLuint aabbBuffer;
    glGenBuffers(1, &aabbBuffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, aabbBuffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, aabbs.size() * sizeof(ChunkAABB), aabbs.data(), GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, aabbBuffer);
    
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_visibilitySSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, m_chunks.size() * sizeof(uint32_t), nullptr, GL_STREAM_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_visibilitySSBO);
    
    glUseProgram(m_frustumCullProgram);
    GLint loc = glGetUniformLocation(m_frustumCullProgram, "uViewProjection");
    glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(viewProj));
    
    glDispatchCompute((m_chunks.size() + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    
    std::vector<uint32_t> visibility(m_chunks.size());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_visibilitySSBO);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, visibility.size() * sizeof(uint32_t), visibility.data());
    
    outVisible.clear();
    for (size_t i = 0; i < m_chunks.size(); i++) {
        if (visibility[i] != 0) {
            outVisible.push_back(m_chunks[i].chunk);
        }
    }
    
    glDeleteBuffers(1, &aabbBuffer);
}

GLuint InstancedQuadRenderer::allocateVBO(size_t sizeBytes)
{
    if (!m_freeVBOPool.empty()) {
        GLuint vbo = m_freeVBOPool.back();
        m_freeVBOPool.pop_back();
        
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        GLint currentSize = 0;
        glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &currentSize);
        
        if (static_cast<size_t>(currentSize) >= sizeBytes) {
            return vbo;
        }
        
        glDeleteBuffers(1, &vbo);
    }
    
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeBytes, nullptr, GL_DYNAMIC_DRAW);
    return vbo;
}

void InstancedQuadRenderer::freeVBO(GLuint vbo)
{
    m_freeVBOPool.push_back(vbo);
}

