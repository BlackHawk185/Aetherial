// InstancedQuadRenderer.cpp - GPU instanced rendering implementation
#include "InstancedQuadRenderer.h"
#include "../World/VoxelChunk.h"
#include "TextureManager.h"
#include "CascadedShadowMap.h"
#include "../Time/DayNightController.h"

#include <iostream>
#include <glm/gtc/type_ptr.hpp>

// Global instance
std::unique_ptr<InstancedQuadRenderer> g_instancedQuadRenderer;

// External globals
extern ShadowMap g_shadowMap;
extern DayNightController* g_dayNightController;

// Vertex shader - MDI version with SSBO transform lookup
static const char* VERTEX_SHADER = R"(
#version 460 core

// Unit quad vertex attributes (shared by all instances)
layout(location = 0) in vec3 aPosition;  // -0.5 to 0.5 range

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

// Uniforms
uniform mat4 uViewProjection;

// Outputs to fragment shader
out vec2 TexCoord;
out vec3 Normal;
out vec3 WorldPos;
flat out uint BlockType;
flat out uint FaceDir;

void main() {
    // Get this chunk's transform using gl_DrawID (which draw command we're in)
    mat4 uChunkTransform = chunkTransforms[gl_DrawID];
    
    // Scale unit quad by instance dimensions
    vec3 scaledPos = vec3(
        aPosition.x * aInstanceWidth,
        aPosition.y * aInstanceHeight,
        0.0
    );
    
    // Rotate to align with face normal
    // Face direction: 0=-Y, 1=+Y, 2=-Z, 3=+Z, 4=-X, 5=+X
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
    
    // Position is the face center - add rotated offset to get vertex position
    vec3 localPos = aInstancePosition + rotatedPos;
    
    // Transform by chunk matrix, then view-projection
    vec4 worldPos4 = uChunkTransform * vec4(localPos, 1.0);
    WorldPos = worldPos4.xyz;
    gl_Position = uViewProjection * worldPos4;
    
    // Texture coordinates
    TexCoord = (aPosition.xy + 0.5) * vec2(aInstanceWidth, aInstanceHeight);
    
    // Pass through normal and block type
    Normal = mat3(uChunkTransform) * aInstanceNormal;
    BlockType = aInstanceBlockType;
    FaceDir = aInstanceFaceDir;
}
)";

// Fragment shader - OPTIMIZED with texture atlas (no branching!)
static const char* FRAGMENT_SHADER = R"(
#version 460 core

in vec2 TexCoord;
in vec3 Normal;
in vec3 WorldPos;
flat in uint BlockType;
flat in uint FaceDir;

uniform sampler2D uTextureStone;
uniform sampler2D uTextureDirt;
uniform sampler2D uTextureGrass;
uniform sampler2D uTextureSand;

// Real-time shadow system (CSM/PCF)
uniform sampler2DArrayShadow uShadowMap;  // Cascaded shadow map array
uniform float uShadowTexel;
uniform vec3 uLightDir;

// Cascade uniforms
uniform mat4 uCascadeVP[2];      // View-projection for each cascade
uniform float uCascadeSplits[2];  // Split distances for cascades
uniform int uNumCascades;         // Number of cascades (typically 2)

// View matrix for view-space depth calculation
uniform mat4 uView;

out vec4 FragColor;

// Poisson disk with 32 samples for high-quality soft shadows (match ModelInstanceRenderer)
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

float sampleShadowPCF(float bias, float viewZ)
{
    // Select cascade based on view-space depth
    int cascadeIndex = 0;
    float viewDepth = abs(viewZ);
    
    if (viewDepth > 64.0) {
        cascadeIndex = 1;  // Far cascade starts at 64 blocks
    }
    
    // Transform to light space for selected cascade
    vec4 lightSpacePos = uCascadeVP[cascadeIndex] * vec4(WorldPos, 1.0);
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    proj = proj * 0.5 + 0.5;
    
    // If outside light frustum, surface receives NO light (dark by default)
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z > 1.0)
        return 0.0;
    
    float current = proj.z - bias;
    
    // Adjust PCF radius based on cascade to maintain consistent world-space blur
    float baseRadius = 128.0;
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

void main() {
    // Sample appropriate texture based on block type
    vec4 texColor;
    if (BlockType == 1u) {
        texColor = texture(uTextureStone, TexCoord);
    } else if (BlockType == 2u) {
        texColor = texture(uTextureDirt, TexCoord);
    } else if (BlockType == 3u) {
        texColor = texture(uTextureGrass, TexCoord);
    } else if (BlockType == 25u) {
        texColor = texture(uTextureSand, TexCoord);
    } else {
        texColor = texture(uTextureStone, TexCoord);  // Default
    }
    
    // Calculate view-space Z for cascade selection
    vec4 viewPos = uView * vec4(WorldPos, 1.0);
    float viewZ = -viewPos.z;
    
    // Slope-scale bias based on surface angle to light
    vec3 N = normalize(Normal);
    vec3 L = normalize(-uLightDir);
    float ndotl = max(dot(N, L), 0.0);
    float bias = max(0.0, 0.0002 * (1.0 - ndotl));
    
    // Sample real-time CSM/PCF shadow
    float visibility = sampleShadowPCF(bias, viewZ);
    
    // Apply shadow visibility (no ambient, pure PCF/CSM like ModelInstanceRenderer)
    vec3 lit = texColor.rgb * visibility;
    FragColor = vec4(lit, texColor.a);
}
)";

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

InstancedQuadRenderer::InstancedQuadRenderer()
    : m_unitQuadVAO(0), m_unitQuadVBO(0), m_unitQuadEBO(0), m_shaderProgram(0), m_depthProgram(0),
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
    createShader();
    createDepthShader();  // For shadow casting
    
    // Load block textures
    extern TextureManager* g_textureManager;
    if (g_textureManager)
    {
        // Try multiple possible paths for textures
        std::vector<std::string> searchPaths = {
            "assets/textures/",
            "../assets/textures/",
            "C:/Users/steve-17/Desktop/game2/assets/textures/"
        };
        
        struct TextureToLoad {
            std::string name;
            std::string filename;
        };
        
        std::vector<TextureToLoad> textures = {
            {"dirt", "dirt.png"},
            {"stone", "stone.png"},
            {"grass", "grass.png"},
            {"sand", "sand.png"}
        };
        
        for (const auto& tex : textures)
        {
            bool loaded = false;
            for (const auto& path : searchPaths)
            {
                std::string fullPath = path + tex.filename;
                GLuint texID = g_textureManager->loadTexture(fullPath, true, true);
                if (texID != 0)
                {
                    std::cout << "   ├─ Loaded texture: " << tex.name << " from " << fullPath << std::endl;
                    loaded = true;
                    break;
                }
            }
            
            if (!loaded)
            {
                std::cerr << "   ⚠️  Failed to load texture: " << tex.name << std::endl;
            }
        }
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

void InstancedQuadRenderer::createShader()
{
    GLuint vertexShader = compileShader(VERTEX_SHADER, GL_VERTEX_SHADER);
    GLuint fragmentShader = compileShader(FRAGMENT_SHADER, GL_FRAGMENT_SHADER);
    
    m_shaderProgram = glCreateProgram();
    glAttachShader(m_shaderProgram, vertexShader);
    glAttachShader(m_shaderProgram, fragmentShader);
    glLinkProgram(m_shaderProgram);
    
    // Check linking
    GLint success;
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_shaderProgram, 512, nullptr, infoLog);
        std::cerr << "❌ Shader linking FAILED:\n" << infoLog << std::endl;
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    // Get uniform locations (no more uChunkTransform - using SSBO)
    m_uViewProjection = glGetUniformLocation(m_shaderProgram, "uViewProjection");
    m_uView = glGetUniformLocation(m_shaderProgram, "uView");
    m_uTextureStone = glGetUniformLocation(m_shaderProgram, "uTextureStone");
    m_uTextureDirt = glGetUniformLocation(m_shaderProgram, "uTextureDirt");
    m_uTextureGrass = glGetUniformLocation(m_shaderProgram, "uTextureGrass");
    m_uTextureSand = glGetUniformLocation(m_shaderProgram, "uTextureSand");
    
    // CSM/PCF shadow uniforms
    m_uShadowMap = glGetUniformLocation(m_shaderProgram, "uShadowMap");
    m_uShadowTexel = glGetUniformLocation(m_shaderProgram, "uShadowTexel");
    m_uLightDir = glGetUniformLocation(m_shaderProgram, "uLightDir");
    m_uCascadeVP = glGetUniformLocation(m_shaderProgram, "uCascadeVP");
    
    std::cout << "   └─ Shader compiled and linked (CSM/PCF enabled)" << std::endl;
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
    
    // Lazy mesh generation - generates mesh on first access if needed
    auto mesh = entry.chunk->getRenderMeshLazy();
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
            // Lazy mesh generation - generates mesh on first access if needed
            auto mesh = entry.chunk->getRenderMeshLazy();
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
    
    // DON'T use getRenderMeshLazy() - it triggers synchronous mesh generation!
    // The mesh should already be generated (either async or by caller)
    auto mesh = entry.chunk->getRenderMesh();
    if (!mesh) {
        return;  // Mesh not ready yet - skip this update
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

void InstancedQuadRenderer::render(const glm::mat4& viewProjection, const glm::mat4& view)
{
    if (m_chunks.empty())
        return;
    
    // Enable face culling to catch winding order issues
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    glUseProgram(m_shaderProgram);
    glUniformMatrix4fv(m_uViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    glUniformMatrix4fv(m_uView, 1, GL_FALSE, glm::value_ptr(view));
    
    // Bind 4 separate textures
    extern TextureManager* g_textureManager;
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_textureManager->getTexture("stone.png"));
    glUniform1i(m_uTextureStone, 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_textureManager->getTexture("dirt.png"));
    glUniform1i(m_uTextureDirt, 1);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_textureManager->getTexture("grass.png"));
    glUniform1i(m_uTextureGrass, 2);
    
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, g_textureManager->getTexture("sand.png"));
    glUniform1i(m_uTextureSand, 3);
    
    // Bind CSM/PCF shadow map (matches ModelInstanceRenderer)
    extern class ShadowMap g_shadowMap;
    extern class DayNightController* g_dayNightController;
    
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_shadowMap.getDepthTexture());
    glUniform1i(m_uShadowMap, 4);
    
    // Shadow texel size for PCF radius calculation
    float shadowTexel = 1.0f / static_cast<float>(g_shadowMap.getSize());
    glUniform1f(m_uShadowTexel, shadowTexel);
    
    // Sun direction from day/night controller
    glm::vec3 sunDir(0.3f, -1.0f, 0.2f);  // Default fallback
    if (g_dayNightController) {
        auto sd = g_dayNightController->getSunDirection();
        sunDir = glm::vec3(sd.x, sd.y, sd.z);
    }
    glUniform3fv(m_uLightDir, 1, glm::value_ptr(sunDir));
    
    // Cascade view-projection matrices
    glm::mat4 cascadeVPs[2];
    for (int i = 0; i < 2; ++i) {
        cascadeVPs[i] = g_shadowMap.getCascade(i).viewProj;
    }
    glUniformMatrix4fv(m_uCascadeVP, 2, GL_FALSE, glm::value_ptr(cascadeVPs[0]));
    
    // Rebuild MDI buffers if dirty (chunks added/removed/updated)
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
    
    // OPTIMIZED: Directly update SSBO buffer with transforms (no temp vector)
    // Map the buffer for writing, copy transforms directly
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
    
    // SINGLE MDI CALL FOR ALL CHUNKS! (GPU-driven rendering)
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 
                                static_cast<GLsizei>(drawCount), 0);
    
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
    
    if (m_shaderProgram != 0) {
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
    }
    
    if (m_depthProgram != 0) {
        glDeleteProgram(m_depthProgram);
        m_depthProgram = 0;
    }
}

// ========== SHADOW DEPTH PASS METHODS ==========

void InstancedQuadRenderer::beginDepthPass(const glm::mat4& lightVP, int cascadeIndex)
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

void InstancedQuadRenderer::endDepthPass(int screenWidth, int screenHeight)
{
    // Shadow map end() will be called by GameClient after all depth rendering
    (void)screenWidth;
    (void)screenHeight;
}

