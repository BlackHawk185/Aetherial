#include "DeferredLightingPass.h"
#include "GBuffer.h"
#include "CascadedShadowMap.h"
#include "HDRFramebuffer.h"
#include <glad/gl.h>
#include <iostream>

DeferredLightingPass g_deferredLighting;

namespace {
    // Full-screen quad vertex shader
    static const char* kVS = R"GLSL(
#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

    // Deferred lighting fragment shader
    static const char* kFS = R"GLSL(
#version 460 core
in vec2 vUV;

// G-buffer textures
uniform sampler2D gAlbedo;
uniform sampler2D gNormal;
uniform sampler2D gPosition;
uniform sampler2D gMetadata;
uniform sampler2D gDepth;

// Shadow mapping
uniform sampler2DArrayShadow uShadowMap;
uniform float uShadowTexel;
uniform mat4 uCascadeVP[2];
uniform int uNumCascades;

// Lighting
uniform vec3 uSunDir;
uniform vec3 uCameraPos;

out vec4 FragColor;

// Cascade distance constants (in world units/blocks)
const float NEAR_CASCADE_END = 128.0;
const float FAR_CASCADE_START = 32.0;

// Poisson disk for PCF soft shadows (32 samples)
const vec2 POISSON[32] = vec2[32](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790),
    vec2(-0.41086680, -0.70708370), vec2(0.70117795, -0.14753070),
    vec2(0.03019430, 0.43220600), vec2(-0.58688340, 0.09974030),
    vec2(0.85595560, -0.33380990), vec2(-0.67121580, 0.59433670),
    vec2(0.29770330, -0.60421660), vec2(-0.10860150, 0.61185800),
    vec2(0.69197035, -0.06798679), vec2(-0.97010720, 0.16373110),
    vec2(0.06372385, 0.37408390), vec2(-0.63902735, -0.56419730),
    vec2(0.56546623, 0.25234550), vec2(-0.23892370, 0.51662970),
    vec2(0.13814290, 0.98162460), vec2(-0.46671060, 0.16780830)
);

float sampleShadowPCF(vec3 worldPos, float bias) {
    // Hard cascade cutoffs with overlap zone
    // Near cascade: 0-128 blocks (hard cutoff at 128)
    // Far cascade: starts at 32 blocks
    // Overlap zone: 32-128 blocks (both render, near wins)
    float distFromCamera = length(worldPos - uCameraPos);
    
    bool useNear = (distFromCamera <= NEAR_CASCADE_END);
    bool useFar = (distFromCamera >= FAR_CASCADE_START);
    bool inOverlap = (distFromCamera >= FAR_CASCADE_START && distFromCamera <= NEAR_CASCADE_END);
    
    float shadow = 1.0;
    
    // Sample near cascade if applicable
    if (useNear) {
        vec4 lightSpacePos0 = uCascadeVP[0] * vec4(worldPos, 1.0);
        vec3 proj0 = lightSpacePos0.xyz / lightSpacePos0.w;
        proj0 = proj0 * 0.5 + 0.5;
        
        if (proj0.x >= 0.0 && proj0.x <= 1.0 && proj0.y >= 0.0 && proj0.y <= 1.0 && proj0.z <= 1.0) {
            float current = proj0.z - bias;
            float radius = 128.0 * uShadowTexel;
            
            float center = texture(uShadowMap, vec4(proj0.xy, 0, current));
            if (center > 0.99) {
                shadow = 1.0;
            } else {
                shadow = center;
                for (int i = 0; i < 32; ++i) {
                    vec2 offset = POISSON[i] * radius;
                    shadow += texture(uShadowMap, vec4(proj0.xy + offset, 0, current));
                }
                shadow /= 33.0;
            }
        }
        
        // If not in overlap zone, return near cascade result
        if (!inOverlap) {
            return shadow;
        }
    }
    
    // Sample far cascade if applicable
    if (useFar) {
        float farShadow = 1.0;
        vec4 lightSpacePos1 = uCascadeVP[1] * vec4(worldPos, 1.0);
        vec3 proj1 = lightSpacePos1.xyz / lightSpacePos1.w;
        proj1 = proj1 * 0.5 + 0.5;
        
        if (proj1.x >= 0.0 && proj1.x <= 1.0 && proj1.y >= 0.0 && proj1.y <= 1.0 && proj1.z <= 1.0) {
            float current = proj1.z - bias;
            float radius = 128.0 * 0.125 * uShadowTexel;
            
            float center = texture(uShadowMap, vec4(proj1.xy, 1, current));
            if (center > 0.99) {
                farShadow = 1.0;
            } else {
                farShadow = center;
                for (int i = 0; i < 32; ++i) {
                    vec2 offset = POISSON[i] * radius;
                    farShadow += texture(uShadowMap, vec4(proj1.xy + offset, 1, current));
                }
                farShadow /= 33.0;
            }
        }
        
        // In overlap zone, combine both cascades (multiply for darker shadows)
        if (inOverlap) {
            return shadow * farShadow;
        } else {
            return farShadow;
        }
    }
    
    return shadow;
}

void main() {
    // Read G-buffer
    vec3 albedo = texture(gAlbedo, vUV).rgb;
    vec3 normal = texture(gNormal, vUV).rgb;
    vec3 worldPos = texture(gPosition, vUV).rgb;
    vec4 metadata = texture(gMetadata, vUV);
    float depth = texture(gDepth, vUV).r;
    
    // Skip pixels with no geometry (depth = 1.0) - let sky pass handle background
    if (depth >= 0.9999) {
        discard;  // Don't render anything for empty pixels
    }
    
    // Shadow mapping only - no Lambert, no ambient
    float bias = 0.0;
    float shadowFactor = sampleShadowPCF(worldPos, bias);
    
    // Final color: albedo modulated by shadow map ONLY
    // Dark by default (shadowFactor = 0), lit only where shadow map says so
    vec3 finalColor = albedo * shadowFactor;
    
    FragColor = vec4(finalColor, 1.0);
}
)GLSL";

    GLuint CompileShader(GLenum type, const char* src) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        
        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[512];
            glGetShaderInfoLog(shader, 512, nullptr, log);
            std::cerr << "❌ Shader compilation failed:\n" << log << std::endl;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint LinkProgram(GLuint vs, GLuint fs) {
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        
        GLint success;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            char log[512];
            glGetProgramInfoLog(program, 512, nullptr, log);
            std::cerr << "❌ Program linking failed:\n" << log << std::endl;
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }
}

DeferredLightingPass::DeferredLightingPass() {
    m_cascadeSplits[0] = 128.0f;
    m_cascadeSplits[1] = 1000.0f;
}

DeferredLightingPass::~DeferredLightingPass() {
    shutdown();
}

bool DeferredLightingPass::initialize() {
    shutdown();
    
    // Compile shaders
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFS);
    
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    
    m_shader = LinkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    if (!m_shader) {
        return false;
    }
    
    // Cache uniform locations
    m_loc_gAlbedo = glGetUniformLocation(m_shader, "gAlbedo");
    m_loc_gNormal = glGetUniformLocation(m_shader, "gNormal");
    m_loc_gPosition = glGetUniformLocation(m_shader, "gPosition");
    m_loc_gMetadata = glGetUniformLocation(m_shader, "gMetadata");
    m_loc_gDepth = glGetUniformLocation(m_shader, "gDepth");
    m_loc_shadowMap = glGetUniformLocation(m_shader, "uShadowMap");
    m_loc_sunDir = glGetUniformLocation(m_shader, "uSunDir");
    m_loc_cameraPos = glGetUniformLocation(m_shader, "uCameraPos");
    m_loc_numCascades = glGetUniformLocation(m_shader, "uNumCascades");
    m_loc_shadowTexel = glGetUniformLocation(m_shader, "uShadowTexel");
    
    // Create full-screen quad
    float quadVertices[] = {
        // Positions   // UVs
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };
    
    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &m_quadVBO);
    
    glBindVertexArray(m_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    
    glBindVertexArray(0);
    
    std::cout << "✅ Deferred lighting pass initialized" << std::endl;
    return true;
}

void DeferredLightingPass::shutdown() {
    if (m_quadVAO) { glDeleteVertexArrays(1, &m_quadVAO); m_quadVAO = 0; }
    if (m_quadVBO) { glDeleteBuffers(1, &m_quadVBO); m_quadVBO = 0; }
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
}

void DeferredLightingPass::setCascadeData(int index, const glm::mat4& viewProj, float splitDistance) {
    if (index >= 0 && index < 2) {
        m_cascadeVP[index] = viewProj;
        m_cascadeSplits[index] = splitDistance;
    }
}

void DeferredLightingPass::render(const glm::vec3& sunDirection, const glm::vec3& cameraPosition) {
    // Bind HDR framebuffer for output
    g_hdrFramebuffer.bind();
    g_hdrFramebuffer.clear();
    
    glUseProgram(m_shader);
    
    // Bind G-buffer textures
    g_gBuffer.bindForLightingPass();
    
    // Set G-buffer sampler uniforms
    if (m_loc_gAlbedo >= 0) glUniform1i(m_loc_gAlbedo, 0);
    if (m_loc_gNormal >= 0) glUniform1i(m_loc_gNormal, 1);
    if (m_loc_gPosition >= 0) glUniform1i(m_loc_gPosition, 2);
    if (m_loc_gMetadata >= 0) glUniform1i(m_loc_gMetadata, 3);
    if (m_loc_gDepth >= 0) glUniform1i(m_loc_gDepth, 4);
    
    // Bind shadow map
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_shadowMap.getDepthTexture());
    if (m_loc_shadowMap >= 0) glUniform1i(m_loc_shadowMap, 5);
    
    // Set lighting uniforms
    if (m_loc_sunDir >= 0) glUniform3fv(m_loc_sunDir, 1, &sunDirection[0]);
    if (m_loc_cameraPos >= 0) glUniform3fv(m_loc_cameraPos, 1, &cameraPosition[0]);
    if (m_loc_numCascades >= 0) glUniform1i(m_loc_numCascades, g_shadowMap.getNumCascades());
    
    float shadowTexel = 1.0f / static_cast<float>(g_shadowMap.getSize());
    if (m_loc_shadowTexel >= 0) glUniform1f(m_loc_shadowTexel, shadowTexel);
    
    // Set cascade matrices
    for (int i = 0; i < 2; ++i) {
        std::string uniformName = "uCascadeVP[" + std::to_string(i) + "]";
        GLint loc = glGetUniformLocation(m_shader, uniformName.c_str());
        if (loc >= 0) {
            glUniformMatrix4fv(loc, 1, GL_FALSE, &m_cascadeVP[i][0][0]);
        }
    }
    
    // Disable depth testing for full-screen quad
    glDisable(GL_DEPTH_TEST);
    
    // Render full-screen quad
    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    
    // Re-enable depth testing
    glEnable(GL_DEPTH_TEST);
    
    glUseProgram(0);
    g_hdrFramebuffer.unbind();
}
