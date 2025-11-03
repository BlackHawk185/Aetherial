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

// Vertex shader - transforms shared unit quad using instance data
static const char* VERTEX_SHADER = R"(
#version 460 core

// Unit quad vertex attributes (shared by all instances)
layout(location = 0) in vec3 aPosition;  // -0.5 to 0.5 range

// Instance attributes (per QuadFace)
layout(location = 1) in vec3 aInstancePosition;
layout(location = 2) in vec3 aInstanceNormal;
layout(location = 3) in float aInstanceWidth;
layout(location = 4) in float aInstanceHeight;
layout(location = 5) in float aInstanceLightmapU;
layout(location = 6) in float aInstanceLightmapV;
layout(location = 7) in uint aInstanceBlockType;
layout(location = 8) in uint aInstanceFaceDir;

// Uniforms
uniform mat4 uViewProjection;
uniform mat4 uChunkTransform;

// Outputs to fragment shader
out vec2 TexCoord;
out vec2 LightmapCoord;
out vec3 Normal;
out vec3 WorldPos;
flat out uint BlockType;
flat out uint FaceDir;

void main() {
    // Unit quad in XY plane with CCW winding from +Z
    // Y is UP in world space, not Z!
    
    // Scale unit quad by instance dimensions
    vec3 scaledPos = vec3(
        aPosition.x * aInstanceWidth,
        aPosition.y * aInstanceHeight,
        0.0
    );
    
    // Rotate to align with face normal
    // Face direction: 0=-Y, 1=+Y, 2=-Z, 3=+Z, 4=-X, 5=+X
    // Y faces are HORIZONTAL (width=X, height=Z)
    // Z and X faces are VERTICAL (height=Y)
    vec3 rotatedPos;
    if (aInstanceFaceDir == 0u) {
        // -Y (bottom): horizontal face, map X→X, Y→Z
        rotatedPos = vec3(scaledPos.x, 0.0, scaledPos.y);
    } else if (aInstanceFaceDir == 1u) {
        // +Y (top): horizontal face, map X→X, Y→Z, flip X for winding
        rotatedPos = vec3(-scaledPos.x, 0.0, scaledPos.y);
    } else if (aInstanceFaceDir == 2u) {
        // -Z (back): vertical face, map X→X, Y→Y, flip X for winding
        rotatedPos = vec3(-scaledPos.x, scaledPos.y, 0.0);
    } else if (aInstanceFaceDir == 3u) {
        // +Z (front): vertical face, map X→X, Y→Y
        rotatedPos = vec3(scaledPos.x, scaledPos.y, 0.0);
    } else if (aInstanceFaceDir == 4u) {
        // -X (left): vertical face, map X→Z, Y→Y
        rotatedPos = vec3(0.0, scaledPos.y, scaledPos.x);
    } else {
        // +X (right): vertical face, map X→Z, Y→Y, flip Z for winding
        rotatedPos = vec3(0.0, scaledPos.y, -scaledPos.x);
    }
    
    // Position is the face center - add rotated offset to get vertex position
    vec3 localPos = aInstancePosition + rotatedPos;
    
    // Transform by chunk matrix, then view-projection
    vec4 worldPos4 = uChunkTransform * vec4(localPos, 1.0);
    WorldPos = worldPos4.xyz;
    gl_Position = uViewProjection * worldPos4;
    
    // Texture coordinates - use the rotated position offset from quad center
    // The quad center (aInstancePosition) represents the face surface
    // We need texture UVs based on the actual vertex position within the quad
    // Map the unit quad space (-0.5 to 0.5) to texture space (0 to width/height)
    TexCoord = (aPosition.xy + 0.5) * vec2(aInstanceWidth, aInstanceHeight);
    
    // Lightmap coordinates - use the pre-calculated UV from quad generation
    LightmapCoord = vec2(aInstanceLightmapU, aInstanceLightmapV);
    
    // Pass through normal and block type
    Normal = mat3(uChunkTransform) * aInstanceNormal;
    BlockType = aInstanceBlockType;
    FaceDir = aInstanceFaceDir;
}
)";

// Fragment shader - real-time CSM/PCF shadows (matches ModelInstanceRenderer)
static const char* FRAGMENT_SHADER = R"(
#version 460 core

in vec2 TexCoord;
in vec2 LightmapCoord;
in vec3 Normal;
in vec3 WorldPos;
flat in uint BlockType;
flat in uint FaceDir;

uniform sampler2D uTexture;      // Dirt
uniform sampler2D uStoneTexture;
uniform sampler2D uGrassTexture;
uniform sampler2D uSandTexture;

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
    vec4 texColor;
    
    // Select texture based on block type
    if (BlockType == 1u) {
        texColor = texture(uStoneTexture, TexCoord);
    } else if (BlockType == 2u) {
        texColor = texture(uTexture, TexCoord);  // Dirt
    } else if (BlockType == 3u) {
        texColor = texture(uGrassTexture, TexCoord);
    } else if (BlockType == 25u) {
        texColor = texture(uSandTexture, TexCoord);
    } else {
        // Placeholder colors for other blocks
        texColor = vec4(0.5, 0.5, 0.5, 1.0);
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

// Depth-only shader for shadow map rendering (no textures, just depth)
static const char* DEPTH_VERTEX_SHADER = R"(
#version 460 core

// Unit quad vertex attributes
layout(location = 0) in vec3 aPosition;

// Instance attributes
layout(location = 1) in vec3 aInstancePosition;
layout(location = 2) in vec3 aInstanceNormal;
layout(location = 3) in float aInstanceWidth;
layout(location = 4) in float aInstanceHeight;
layout(location = 5) in float aInstanceLightmapU;
layout(location = 6) in float aInstanceLightmapV;
layout(location = 7) in uint aInstanceBlockType;
layout(location = 8) in uint aInstanceFaceDir;

uniform mat4 uLightVP;
uniform mat4 uChunkTransform;

void main() {
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
    : m_unitQuadVAO(0), m_unitQuadVBO(0), m_unitQuadEBO(0), m_shaderProgram(0), m_depthProgram(0)
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
    
    std::cout << "✅ InstancedQuadRenderer initialized" << std::endl;
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
    
    // Get uniform locations
    m_uViewProjection = glGetUniformLocation(m_shaderProgram, "uViewProjection");
    m_uView = glGetUniformLocation(m_shaderProgram, "uView");
    m_uTexture = glGetUniformLocation(m_shaderProgram, "uTexture");
    m_uStoneTexture = glGetUniformLocation(m_shaderProgram, "uStoneTexture");
    m_uGrassTexture = glGetUniformLocation(m_shaderProgram, "uGrassTexture");
    m_uSandTexture = glGetUniformLocation(m_shaderProgram, "uSandTexture");
    
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
    
    // Get uniform locations
    m_depth_uLightVP = glGetUniformLocation(m_depthProgram, "uLightVP");
    
    std::cout << "   └─ Depth shader compiled and linked (for shadow casting)" << std::endl;
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
    
    // Check if chunk is already registered - update instead of duplicate
    for (auto& entry : m_chunks) {
        if (entry.chunk == chunk) {
            entry.transform = transform;
            uploadChunkInstances(entry);
            return;
        }
    }
    
    // Not registered yet - create new entry
    ChunkEntry entry;
    entry.chunk = chunk;
    entry.transform = transform;
    entry.instanceCount = 0;
    
    // Create instance VBO
    glGenBuffers(1, &entry.instanceVBO);
    
    // Upload instance data
    uploadChunkInstances(entry);
    
    m_chunks.push_back(entry);
}

void InstancedQuadRenderer::uploadChunkInstances(ChunkEntry& entry)
{
    if (!entry.chunk)
    {
        entry.instanceCount = 0;
        return;
    }
    
    const auto& mesh = entry.chunk->getMesh();
    const auto& quads = mesh.quads;
    entry.instanceCount = quads.size();
    
    if (entry.instanceCount == 0)
        return;
    
    glBindBuffer(GL_ARRAY_BUFFER, entry.instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, quads.size() * sizeof(QuadFace), quads.data(), GL_DYNAMIC_DRAW);
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
            uploadChunkInstances(entry);
            return;
        }
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
    
    // Bind textures
    extern TextureManager* g_textureManager;
    
    glActiveTexture(GL_TEXTURE0);
    GLuint dirtTex = g_textureManager->getTexture("dirt.png");
    glBindTexture(GL_TEXTURE_2D, dirtTex);
    glUniform1i(m_uTexture, 0);
    
    glActiveTexture(GL_TEXTURE1);
    GLuint stoneTex = g_textureManager->getTexture("stone.png");
    glBindTexture(GL_TEXTURE_2D, stoneTex);
    glUniform1i(m_uStoneTexture, 1);
    
    glActiveTexture(GL_TEXTURE2);
    GLuint grassTex = g_textureManager->getTexture("grass.png");
    glBindTexture(GL_TEXTURE_2D, grassTex);
    glUniform1i(m_uGrassTexture, 2);
    
    glActiveTexture(GL_TEXTURE3);
    GLuint sandTex = g_textureManager->getTexture("sand.png");
    glBindTexture(GL_TEXTURE_2D, sandTex);
    glUniform1i(m_uSandTexture, 3);
    
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
    
    // Bind unit quad
    glBindVertexArray(m_unitQuadVAO);
    
    // Render each chunk
    for (const auto& entry : m_chunks) {
        if (!entry.chunk || entry.instanceCount == 0)
            continue;
        
        // Set chunk transform
        GLint uChunkTransform = glGetUniformLocation(m_shaderProgram, "uChunkTransform");
        glUniformMatrix4fv(uChunkTransform, 1, GL_FALSE, glm::value_ptr(entry.transform));
        
        // Bind instance data
        glBindBuffer(GL_ARRAY_BUFFER, entry.instanceVBO);
        
        // Instance attributes (QuadFace struct layout)
        size_t offset = 0;
        
        // aInstancePosition (vec3)
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(1, 1);
        offset += sizeof(Vec3);
        
        // aInstanceNormal (vec3)
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(2, 1);
        offset += sizeof(Vec3);
        
        // aInstanceWidth (float)
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(3, 1);
        offset += sizeof(float);
        
        // aInstanceHeight (float)
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(4, 1);
        offset += sizeof(float);
        
        // aInstanceLightmapU (float)
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(5, 1);
        offset += sizeof(float);
        
        // aInstanceLightmapV (float)
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(6, 1);
        offset += sizeof(float);
        
        // aInstanceBlockType (uint8_t)
        glEnableVertexAttribArray(7);
        glVertexAttribIPointer(7, 1, GL_UNSIGNED_BYTE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(7, 1);
        offset += sizeof(uint8_t);
        
        // aInstanceFaceDir (uint8_t)
        glEnableVertexAttribArray(8);
        glVertexAttribIPointer(8, 1, GL_UNSIGNED_BYTE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(8, 1);
        
        // Draw instanced
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0, entry.instanceCount);
    }
    
    glBindVertexArray(0);
}

void InstancedQuadRenderer::clear()
{
    for (auto& entry : m_chunks) {
        if (entry.instanceVBO != 0) {
            glDeleteBuffers(1, &entry.instanceVBO);
        }
    }
    m_chunks.clear();
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
    
    // Bind unit quad
    glBindVertexArray(m_unitQuadVAO);
    
    // Render each chunk's quads into shadow map
    for (const auto& entry : m_chunks) {
        if (!entry.chunk || entry.instanceCount == 0)
            continue;
        
        // Set chunk transform
        GLint uChunkTransform = glGetUniformLocation(m_depthProgram, "uChunkTransform");
        glUniformMatrix4fv(uChunkTransform, 1, GL_FALSE, glm::value_ptr(entry.transform));
        
        // Bind instance data (same layout as forward rendering)
        glBindBuffer(GL_ARRAY_BUFFER, entry.instanceVBO);
        
        size_t offset = 0;
        
        // aInstancePosition (vec3)
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(1, 1);
        offset += sizeof(Vec3);
        
        // aInstanceNormal (vec3)
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(2, 1);
        offset += sizeof(Vec3);
        
        // aInstanceWidth (float)
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(3, 1);
        offset += sizeof(float);
        
        // aInstanceHeight (float)
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(4, 1);
        offset += sizeof(float);
        
        // aInstanceLightmapU (float)
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(5, 1);
        offset += sizeof(float);
        
        // aInstanceLightmapV (float)
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(6, 1);
        offset += sizeof(float);
        
        // aInstanceBlockType (uint8_t)
        glEnableVertexAttribArray(7);
        glVertexAttribIPointer(7, 1, GL_UNSIGNED_BYTE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(7, 1);
        offset += sizeof(uint8_t);
        
        // aInstanceFaceDir (uint8_t)
        glEnableVertexAttribArray(8);
        glVertexAttribIPointer(8, 1, GL_UNSIGNED_BYTE, sizeof(QuadFace), (void*)offset);
        glVertexAttribDivisor(8, 1);
        
        // Draw instanced (same as forward pass, but depth shader)
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0, entry.instanceCount);
    }
    
    glBindVertexArray(0);
}

void InstancedQuadRenderer::endDepthPass(int screenWidth, int screenHeight)
{
    // Shadow map end() will be called by GameClient after all depth rendering
    (void)screenWidth;
    (void)screenHeight;
}

