// InstancedQuadRenderer.cpp - GPU instanced rendering implementation
#include "InstancedQuadRenderer.h"
#include "../World/VoxelChunk.h"
#include "TextureManager.h"
#include "CascadedShadowMap.h"
#include "../Time/DayNightController.h"
#include "../Profiling/Profiler.h"

#include <iostream>
#include <glm/gtc/type_ptr.hpp>

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

// Depth-only shader for shadow map rendering - MDI version with SSBO
static const char* DEPTH_VERTEX_SHADER = R"(
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

// Chunk transforms in SSBO (accessed via gl_DrawID)
layout(std430, binding = 0) readonly buffer TransformBuffer {
    mat4 chunkTransforms[];
};

uniform mat4 uLightVP;

void main() {
    // Get this chunk's transform using gl_DrawID
    mat4 uChunkTransform = chunkTransforms[gl_DrawID];
    
    // Same rotation logic as forward shader
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

void main() {
    // Depth is written automatically - no color output needed
}
)";

// G-Buffer shaders (deferred rendering)
static const char* GBUFFER_VERTEX_SHADER = R"(
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

// Chunk transforms in SSBO
layout(std430, binding = 0) readonly buffer TransformBuffer {
    mat4 chunkTransforms[];
};

uniform mat4 uViewProjection;

out vec2 TexCoord;
out vec3 Normal;
out vec3 WorldPos;
flat out uint BlockType;
flat out uint FaceDir;

void main() {
    mat4 uChunkTransform = chunkTransforms[gl_DrawID];
    
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

InstancedQuadRenderer::InstancedQuadRenderer()
    : m_unitQuadVAO(0), m_unitQuadVBO(0), m_unitQuadEBO(0), m_gbufferProgram(0), m_depthProgram(0),
      m_globalVAO(0), m_globalInstanceVBO(0), m_indirectCommandBuffer(0), m_transformSSBO(0), 
      m_mdiDirty(false), m_totalAllocatedInstances(0)
{
}

InstancedQuadRenderer::~InstancedQuadRenderer()
{
    shutdown();
}

bool InstancedQuadRenderer::initialize()
{
    std::cout << "🎨 Initializing InstancedQuadRenderer..." << std::endl;
    
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
    
    // Create global VAO for MDI (will be configured when chunks are registered)
    glGenVertexArrays(1, &m_globalVAO);
    glGenBuffers(1, &m_globalInstanceVBO);
    glGenBuffers(1, &m_indirectCommandBuffer);
    glGenBuffers(1, &m_transformSSBO);
    
    std::cout << "✅ InstancedQuadRenderer initialized with MDI support" << std::endl;
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
    
    std::cout << "   ├─ Unit quad created (4 vertices, 6 indices)" << std::endl;
}

void InstancedQuadRenderer::createDepthShader()
{
    GLuint vertexShader = compileShader(DEPTH_VERTEX_SHADER, GL_VERTEX_SHADER);
    GLuint fragmentShader = compileShader(DEPTH_FRAGMENT_SHADER, GL_FRAGMENT_SHADER);
    
    m_depthProgram = glCreateProgram();
    glAttachShader(m_depthProgram, vertexShader);
    glAttachShader(m_depthProgram, fragmentShader);
    glLinkProgram(m_depthProgram);
    
    // Check linking
    GLint success;
    glGetProgramiv(m_depthProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_depthProgram, 512, nullptr, infoLog);
        std::cerr << "❌ Depth shader linking FAILED:\n" << infoLog << std::endl;
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    // Get uniform locations (no more uChunkTransform - using SSBO)
    m_depth_uLightVP = glGetUniformLocation(m_depthProgram, "uLightVP");
    
    std::cout << "   └─ Depth shader compiled and linked (MDI + SSBO)" << std::endl;
}

void InstancedQuadRenderer::createGBufferShader()
{
    GLuint vertexShader = compileShader(GBUFFER_VERTEX_SHADER, GL_VERTEX_SHADER);
    GLuint fragmentShader = compileShader(GBUFFER_FRAGMENT_SHADER, GL_FRAGMENT_SHADER);
    
    m_gbufferProgram = glCreateProgram();
    glAttachShader(m_gbufferProgram, vertexShader);
    glAttachShader(m_gbufferProgram, fragmentShader);
    glLinkProgram(m_gbufferProgram);
    
    // Check linking
    GLint success;
    glGetProgramiv(m_gbufferProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_gbufferProgram, 512, nullptr, infoLog);
        std::cerr << "❌ G-buffer shader linking FAILED:\n" << infoLog << std::endl;
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    // Get uniform locations
    m_gbuffer_uViewProjection = glGetUniformLocation(m_gbufferProgram, "uViewProjection");
    m_gbuffer_uBlockTextures = glGetUniformLocation(m_gbufferProgram, "uBlockTextures");
    
    std::cout << "   └─ G-buffer shader compiled and linked (deferred rendering)" << std::endl;
}

bool InstancedQuadRenderer::loadBlockTextureArray()
{
    std::cout << "🎨 Loading block texture array..." << std::endl;
    
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
    
    // Load and upload each texture directly
    int successCount = 0;
    int failCount = 0;
    
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
            else
            {
                std::cerr << "   ⚠️  Layer " << i << " (" << blockTextureFiles[i] << "): Wrong size " 
                          << texData.width << "x" << texData.height << " (expected 32x32)" << std::endl;
                failCount++;
            }
        }
        else
        {
            std::cerr << "   ❌ Layer " << i << " (" << blockTextureFiles[i] << "): Failed to load" << std::endl;
            failCount++;
        }
    }
    
    // Set texture parameters (pixel art style - nearest neighbor)
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    
    std::cout << "   ✅ Texture array created: " << successCount << "/46 loaded";
    if (failCount > 0) {
        std::cout << " (" << failCount << " failed)";
    }
    std::cout << std::endl;
    
    return successCount > 0; // At least one texture must load
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
    if (!chunk)
    {
        std::cerr << "⚠️ Attempted to register null chunk!" << std::endl;
        return;
    }
    
    // EVENT-DRIVEN: Set up callback for immediate GPU upload on mesh changes
    // Do this FIRST before checking if already registered to ensure callback is always set
    chunk->setMeshUpdateCallback([this](VoxelChunk* modifiedChunk) {
        // Find the chunk entry
        for (auto& chunkEntry : m_chunks) {
            if (chunkEntry.chunk == modifiedChunk) {
                // Immediate GPU upload (zero latency)
                this->updateSingleChunkGPU(chunkEntry);
                return;
            }
        }
    });
    
    // Check if chunk is already registered - update instead of duplicate
    for (auto& entry : m_chunks) {
        if (entry.chunk == chunk) {
            entry.transform = transform;
            m_mdiDirty = true;  // Mark for rebuild
            return;
        }
    }
    
    // Not registered yet - create new entry
    ChunkEntry entry;
    entry.chunk = chunk;
    entry.transform = transform;
    entry.instanceCount = 0;
    entry.baseInstance = 0;  // Will be set during rebuildMDIBuffers
    entry.allocatedSlots = 0;  // Will be calculated during rebuildMDIBuffers
    
    m_chunks.push_back(entry);
    m_mdiDirty = true;  // Mark for rebuild
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
    
    // Instance data is now managed by rebuildMDIBuffers, not per-chunk
    m_mdiDirty = true;
}

void InstancedQuadRenderer::updateChunkTransform(VoxelChunk* chunk, const glm::mat4& transform)
{
    for (auto& entry : m_chunks) {
        if (entry.chunk == chunk) {
            entry.transform = transform;
            return;
        }
    }
}

void InstancedQuadRenderer::rebuildChunk(VoxelChunk* chunk)
{
    for (auto& entry : m_chunks) {
        if (entry.chunk == chunk) {
            // Use fast partial update instead of full rebuild
            updateSingleChunkGPU(entry);
            return;
        }
    }
}

// Calculate padded allocation size for a chunk to allow for growth
size_t InstancedQuadRenderer::calculateChunkSlots(size_t quadCount)
{
    // Allocate 150% of current quad count (rounded up to nearest 256)
    // This allows for block additions without full buffer rebuild
    size_t padded = (quadCount * 3) / 2;  // 150%
    size_t alignment = 256;
    return ((padded + alignment - 1) / alignment) * alignment;
}

void InstancedQuadRenderer::rebuildMDIBuffers()
{
    if (m_chunks.empty()) return;
    
    // 1. Calculate total instance count WITH PADDING for future growth
    size_t totalActiveInstances = 0;
    m_totalAllocatedInstances = 0;
    
    for (auto& entry : m_chunks) {
        if (entry.chunk) {
            // Get existing mesh - don't generate synchronously (async system handles it)
            auto mesh = entry.chunk->getRenderMesh();
            if (mesh) {
                entry.instanceCount = mesh->quads.size();
                entry.allocatedSlots = calculateChunkSlots(entry.instanceCount);
                entry.baseInstance = m_totalAllocatedInstances;
                
                totalActiveInstances += entry.instanceCount;
                m_totalAllocatedInstances += entry.allocatedSlots;
            } else {
                entry.instanceCount = 0;
                entry.allocatedSlots = 0;
            }
        }
    }
    
    if (totalActiveInstances == 0) return;
    
    // 2. Build merged instance buffer with PADDED allocation
    std::vector<QuadFace> mergedInstances;
    mergedInstances.resize(m_totalAllocatedInstances);  // Pre-allocate with padding
    
    size_t writeOffset = 0;
    for (const auto& entry : m_chunks) {
        if (entry.chunk && entry.instanceCount > 0) {
            // Thread-safe atomic mesh access - no mutex needed!
            auto mesh = entry.chunk->getRenderMesh();
            if (mesh && entry.baseInstance == writeOffset) {
                // Copy active quads
                std::copy(mesh->quads.begin(), mesh->quads.end(), 
                         mergedInstances.begin() + writeOffset);
                writeOffset += entry.allocatedSlots;  // Skip to next chunk's slot
            }
        }
    }
    
    // 3. Upload merged instance data to GPU (now with padding for future updates)
    glBindBuffer(GL_ARRAY_BUFFER, m_globalInstanceVBO);
    glBufferData(GL_ARRAY_BUFFER, m_totalAllocatedInstances * sizeof(QuadFace), 
                 mergedInstances.data(), GL_DYNAMIC_DRAW);
    
    // 4. Configure global VAO (one-time setup, or when first built)
    static bool vaoConfigured = false;
    if (!vaoConfigured) {
        glBindVertexArray(m_globalVAO);
        
        // Bind shared unit quad (vertex attribute 0)
        glBindBuffer(GL_ARRAY_BUFFER, m_unitQuadVBO);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_unitQuadEBO);
        
        // Bind merged instance buffer
        glBindBuffer(GL_ARRAY_BUFFER, m_globalInstanceVBO);
        
        // Pre-configure all instance vertex attributes (QuadFace: 36 bytes)
        size_t offset = 0;
        
        // Location 1: position (Vec3)
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(1, 1);
        offset += sizeof(Vec3);
        
        // Location 2: normal (Vec3)
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(2, 1);
        offset += sizeof(Vec3);
        
        // Location 3: width (float)
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(3, 1);
        offset += sizeof(float);
        
        // Location 4: height (float)
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(4, 1);
        offset += sizeof(float);
        
        // Location 5: blockType (uint8_t)
        glEnableVertexAttribArray(5);
        glVertexAttribIPointer(5, 1, GL_UNSIGNED_BYTE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(5, 1);
        offset += sizeof(uint8_t);
        
        // Location 6: faceDir (uint8_t)
        glEnableVertexAttribArray(6);
        glVertexAttribIPointer(6, 1, GL_UNSIGNED_BYTE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(6, 1);
        
        glBindVertexArray(0);
        vaoConfigured = true;
    }
    
    // 5. Build indirect command buffer and transform array (MUST match 1:1)
    std::vector<DrawElementsIndirectCommand> commands;
    std::vector<glm::mat4> transforms;
    commands.reserve(m_chunks.size());
    transforms.reserve(m_chunks.size());
    
    for (const auto& entry : m_chunks) {
        if (entry.instanceCount > 0) {
            DrawElementsIndirectCommand cmd;
            cmd.count = 6;  // Quad indices
            cmd.instanceCount = static_cast<uint32_t>(entry.instanceCount);
            cmd.firstIndex = 0;
            cmd.baseVertex = 0;
            cmd.baseInstance = static_cast<uint32_t>(entry.baseInstance);
            commands.push_back(cmd);
            
            // CRITICAL: Transform array must match command array 1:1
            transforms.push_back(entry.transform);
        }
    }
    
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectCommandBuffer);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, commands.size() * sizeof(DrawElementsIndirectCommand),
                 commands.data(), GL_DYNAMIC_DRAW);
    
    // 6. Upload chunk transforms to SSBO (now matches commands array)
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_transformSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, transforms.size() * sizeof(glm::mat4),
                 transforms.data(), GL_DYNAMIC_DRAW);
    
    m_mdiDirty = false;
}

// CRITICAL OPTIMIZATION: Update single chunk without rebuilding entire buffer
void InstancedQuadRenderer::updateSingleChunkGPU(ChunkEntry& entry)
{
    if (!entry.chunk) return;
    
    auto t_start = std::chrono::high_resolution_clock::now();
    
    // Get mesh - use lazy generation only if incremental updates aren't enabled yet
    // This handles the case where a block is modified before async mesh gen completes
    auto mesh = entry.chunk->getRenderMesh();
    if (!mesh || entry.chunk->isMeshDirty()) {
        // Mesh not ready or needs regeneration - use lazy generation as fallback
        mesh = entry.chunk->getRenderMeshLazy();
    }
    if (!mesh) {
        return;  // Mesh still not available - skip this update
    }
    
    size_t newQuadCount = mesh->quads.size();
    
    auto t_check = std::chrono::high_resolution_clock::now();
    
    // Check if we can fit within allocated space
    if (newQuadCount <= entry.allocatedSlots) {
        // FAST PATH: Partial update with glBufferSubData
        auto t_upload_start = std::chrono::high_resolution_clock::now();
        
        glBindBuffer(GL_ARRAY_BUFFER, m_globalInstanceVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 
                       entry.baseInstance * sizeof(QuadFace),
                       newQuadCount * sizeof(QuadFace),
                       mesh->quads.data());
        
        auto t_upload_end = std::chrono::high_resolution_clock::now();
        auto t_cmd_start = std::chrono::high_resolution_clock::now();
        
        // Update instance count in indirect command buffer
        size_t chunkIndex = &entry - m_chunks.data();
        DrawElementsIndirectCommand cmd;
        cmd.count = 6;
        cmd.instanceCount = static_cast<uint32_t>(newQuadCount);
        cmd.firstIndex = 0;
        cmd.baseVertex = 0;
        cmd.baseInstance = static_cast<uint32_t>(entry.baseInstance);
        
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectCommandBuffer);
        glBufferSubData(GL_DRAW_INDIRECT_BUFFER,
                       chunkIndex * sizeof(DrawElementsIndirectCommand),
                       sizeof(DrawElementsIndirectCommand),
                       &cmd);
        
        auto t_cmd_end = std::chrono::high_resolution_clock::now();
        
        entry.instanceCount = newQuadCount;
        
        auto t_end = std::chrono::high_resolution_clock::now();
        
        auto check_ms = std::chrono::duration<double, std::milli>(t_check - t_start).count();
        auto upload_ms = std::chrono::duration<double, std::milli>(t_upload_end - t_upload_start).count();
        auto cmd_ms = std::chrono::duration<double, std::milli>(t_cmd_end - t_cmd_start).count();
        auto total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
        
        size_t uploadBytes = newQuadCount * sizeof(QuadFace);
        std::cout << "🚀 GPU UPLOAD: " << total_ms << "ms (Check=" << check_ms 
                  << "ms, Upload=" << upload_ms << "ms [" << (uploadBytes/1024) << "KB], Cmd=" 
                  << cmd_ms << "ms) Quads=" << newQuadCount << std::endl;
    } else {
        // SLOW PATH: Chunk grew beyond allocated space - need full rebuild
        std::cout << "⚠️ Chunk grew beyond padding - triggering full rebuild" << std::endl;
        m_mdiDirty = true;
    }
}

void InstancedQuadRenderer::renderToGBuffer(const glm::mat4& viewProjection, const glm::mat4& view)
{
    (void)view;  // Not needed for G-buffer pass
    
    if (m_chunks.empty())
        return;
    
    // Enable face culling
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    glUseProgram(m_gbufferProgram);
    glUniformMatrix4fv(m_gbuffer_uViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    
    // Bind texture array
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_blockTextureArray);
    glUniform1i(m_gbuffer_uBlockTextures, 0);
    
    // Rebuild MDI buffers if dirty
    if (m_mdiDirty) {
        rebuildMDIBuffers();
    }
    
    // Count non-empty draws
    size_t drawCount = 0;
    for (const auto& entry : m_chunks) {
        if (entry.instanceCount > 0) drawCount++;
    }
    
    if (drawCount == 0) {
        return;
    }
    
    // Update transforms
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_transformSSBO);
    glm::mat4* mappedTransforms = (glm::mat4*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_WRITE_ONLY);
    if (mappedTransforms) {
        size_t index = 0;
        for (const auto& entry : m_chunks) {
            if (entry.instanceCount > 0) {
                mappedTransforms[index++] = entry.transform;
            }
        }
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    
    // Bind global VAO and transform SSBO
    glBindVertexArray(m_globalVAO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_transformSSBO);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectCommandBuffer);
    
    // SINGLE MDI CALL - writes to G-buffer
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 
                                static_cast<GLsizei>(drawCount), 0);
    
    glBindVertexArray(0);
}

void InstancedQuadRenderer::renderToGBufferCulled(const glm::mat4& viewProjection, const glm::mat4& view, const std::vector<VoxelChunk*>& visibleChunks)
{
    PROFILE_SCOPE("QuadRenderer_GBuffer");
    (void)view;  // Not needed for G-buffer pass
    
    if (m_chunks.empty() || visibleChunks.empty())
        return;
    
    // Enable face culling
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    glUseProgram(m_gbufferProgram);
    glUniformMatrix4fv(m_gbuffer_uViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    
    // Bind texture array
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_blockTextureArray);
    glUniform1i(m_gbuffer_uBlockTextures, 0);
    
    // Rebuild MDI buffers if dirty
    if (m_mdiDirty) {
        rebuildMDIBuffers();
    }
    
    // Build list of visible chunk entries and transforms
    std::vector<const ChunkEntry*> visibleEntries;
    std::vector<glm::mat4> transforms;
    visibleEntries.reserve(visibleChunks.size());
    transforms.reserve(visibleChunks.size());
    
    {
        PROFILE_SCOPE("FindVisibleChunks");
        for (VoxelChunk* chunk : visibleChunks) {
            // Find chunk in our registered list
            for (const auto& entry : m_chunks) {
                if (entry.chunk == chunk && entry.instanceCount > 0) {
                    visibleEntries.push_back(&entry);
                    transforms.push_back(entry.transform);
                    break;
                }
            }
        }
    }
    
    if (visibleEntries.empty()) return;
    
    // Build draw commands for visible chunks
    std::vector<DrawElementsIndirectCommand> commands;
    commands.reserve(visibleEntries.size());
    
    {
        PROFILE_SCOPE("UploadGPUData");
        // Upload transforms for visible chunks
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_transformSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, transforms.size() * sizeof(glm::mat4), transforms.data());
        
        for (size_t i = 0; i < visibleEntries.size(); ++i) {
            const ChunkEntry* entry = visibleEntries[i];
            commands.push_back({
                6,                                    // count (6 indices per quad)
                static_cast<uint32_t>(entry->instanceCount),  // instanceCount
                0,                                    // firstIndex
                0,                                    // baseVertex
                static_cast<uint32_t>(entry->baseInstance)    // baseInstance
            });
        }
        
        // Upload culled draw commands
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectCommandBuffer);
        glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, commands.size() * sizeof(DrawElementsIndirectCommand), commands.data());
    }
    
    {
        PROFILE_SCOPE("ActualGPUDraw");
        // Bind global VAO and transform SSBO
        glBindVertexArray(m_globalVAO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_transformSSBO);
        
        // SINGLE MDI CALL with only visible chunks
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 
                                    static_cast<GLsizei>(commands.size()), 0);
    }
    
    glBindVertexArray(0);
}

void InstancedQuadRenderer::clear()
{
    // No per-chunk VAOs/VBOs to delete - just clear the list
    m_chunks.clear();
    m_mdiDirty = true;  // Mark for rebuild
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
    if (m_globalVAO != 0) {
        glDeleteVertexArrays(1, &m_globalVAO);
        m_globalVAO = 0;
    }
    
    if (m_globalInstanceVBO != 0) {
        glDeleteBuffers(1, &m_globalInstanceVBO);
        m_globalInstanceVBO = 0;
    }
    
    if (m_indirectCommandBuffer != 0) {
        glDeleteBuffers(1, &m_indirectCommandBuffer);
        m_indirectCommandBuffer = 0;
    }
    
    if (m_transformSSBO != 0) {
        glDeleteBuffers(1, &m_transformSSBO);
        m_transformSSBO = 0;
    }
    
    if (m_gbufferProgram != 0) {
        glDeleteProgram(m_gbufferProgram);
        m_gbufferProgram = 0;
    }
    
    if (m_depthProgram != 0) {
        glDeleteProgram(m_depthProgram);
        m_depthProgram = 0;
    }
}

// ========== SHADOW DEPTH PASS METHODS ==========

void InstancedQuadRenderer::beginDepthPass(const glm::mat4& lightVP, int /*cascadeIndex*/)
{
    if (m_depthProgram == 0) return;  // Not initialized
    
    // Shadow map begin() already called by GameClient - just set shader uniforms
    glUseProgram(m_depthProgram);
    if (m_depth_uLightVP != -1) {
        glUniformMatrix4fv(m_depth_uLightVP, 1, GL_FALSE, glm::value_ptr(lightVP));
    }
}

void InstancedQuadRenderer::renderDepth()
{
    if (m_depthProgram == 0 || m_chunks.empty()) return;
    
    // Rebuild MDI buffers if dirty
    if (m_mdiDirty) {
        rebuildMDIBuffers();
    }
    
    // Count non-empty draws
    size_t drawCount = 0;
    for (const auto& entry : m_chunks) {
        if (entry.instanceCount > 0) drawCount++;
    }
    
    if (drawCount == 0) {
        return;
    }
    
    // Update transforms every frame (islands are moving!)
    std::vector<glm::mat4> transforms;
    transforms.reserve(drawCount);
    for (const auto& entry : m_chunks) {
        if (entry.instanceCount > 0) {
            transforms.push_back(entry.transform);
        }
    }
    
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_transformSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, transforms.size() * sizeof(glm::mat4), transforms.data());
    
    // Bind global VAO and transform SSBO
    glBindVertexArray(m_globalVAO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_transformSSBO);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectCommandBuffer);
    
    // SINGLE MDI CALL FOR ALL CHUNKS! (shadows)
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 
                                static_cast<GLsizei>(drawCount), 0);
    
    glBindVertexArray(0);
}

void InstancedQuadRenderer::renderDepthCulled(const std::vector<VoxelChunk*>& visibleChunks)
{
    if (m_depthProgram == 0 || m_chunks.empty() || visibleChunks.empty()) return;
    
    // Rebuild MDI buffers if dirty
    if (m_mdiDirty) {
        rebuildMDIBuffers();
    }
    
    // Build list of visible chunk entries and transforms
    std::vector<const ChunkEntry*> visibleEntries;
    std::vector<glm::mat4> transforms;
    visibleEntries.reserve(visibleChunks.size());
    transforms.reserve(visibleChunks.size());
    
    for (VoxelChunk* chunk : visibleChunks) {
        // Find chunk in our registered list
        for (const auto& entry : m_chunks) {
            if (entry.chunk == chunk && entry.instanceCount > 0) {
                visibleEntries.push_back(&entry);
                transforms.push_back(entry.transform);
                break;
            }
        }
    }
    
    if (visibleEntries.empty()) return;
    
    // Upload transforms for visible chunks
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_transformSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, transforms.size() * sizeof(glm::mat4), transforms.data());
    
    // Build draw commands for visible chunks only
    std::vector<DrawElementsIndirectCommand> commands;
    commands.reserve(visibleEntries.size());
    
    for (size_t i = 0; i < visibleEntries.size(); ++i) {
        const ChunkEntry* entry = visibleEntries[i];
        commands.push_back({
            6,                                    // count (6 indices per quad)
            static_cast<uint32_t>(entry->instanceCount),  // instanceCount
            0,                                    // firstIndex
            0,                                    // baseVertex
            static_cast<uint32_t>(entry->baseInstance)    // baseInstance
        });
    }
    
    // Upload culled draw commands
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectCommandBuffer);
    glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, commands.size() * sizeof(DrawElementsIndirectCommand), commands.data());
    
    // Bind and draw
    glBindVertexArray(m_globalVAO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_transformSSBO);
    
    // MDI call with only visible chunks
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 
                                static_cast<GLsizei>(commands.size()), 0);
    
    glBindVertexArray(0);
}

void InstancedQuadRenderer::endDepthPass(int screenWidth, int screenHeight)
{
    // Shadow map end() will be called by GameClient after all depth rendering
    (void)screenWidth;
    (void)screenHeight;
}

