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

// Light mapping (dark by default, lit where depth test passes)
uniform sampler2DArrayShadow uLightMap;  // 4 cascades: [0,1]=sun, [2,3]=moon
uniform float uLightTexel;
uniform mat4 uCascadeVP[4];
uniform int uNumCascades;
uniform float uCascadeOrthoSizes[4];
uniform float uDitherStrength;

// Lighting
uniform vec3 uSunDir;
uniform vec3 uMoonDir;
uniform float uSunIntensity;
uniform float uMoonIntensity;
uniform vec3 uCameraPos;

out vec4 FragColor;

// Cascade split: hard cutoff at 128 blocks (no blending)
const float CASCADE_SPLIT = 128.0;

// Poisson disk for PCF soft lighting (64 samples)
const vec2 POISSON[64] = vec2[64](
    vec2(-0.613392, 0.617481), vec2(0.170019, -0.040254), vec2(-0.299417, 0.791925),
    vec2(0.645680, 0.493210), vec2(-0.651784, 0.717887), vec2(0.421003, 0.027070),
    vec2(-0.817194, -0.271096), vec2(-0.705374, -0.668203), vec2(0.977050, -0.108615),
    vec2(0.063326, 0.142369), vec2(0.203528, 0.214331), vec2(-0.667531, 0.326090),
    vec2(-0.098422, -0.295755), vec2(-0.885922, 0.215369), vec2(0.566637, 0.605213),
    vec2(0.039766, -0.396100), vec2(0.751946, 0.453352), vec2(0.078707, -0.715323),
    vec2(-0.075838, -0.529344), vec2(0.724479, -0.580798), vec2(0.222999, -0.215125),
    vec2(-0.467574, -0.405438), vec2(-0.248268, -0.814753), vec2(0.354411, -0.887570),
    vec2(0.175817, 0.382366), vec2(0.487472, -0.063082), vec2(-0.084078, 0.898312),
    vec2(0.488876, -0.783441), vec2(0.470016, 0.217933), vec2(-0.696890, -0.549791),
    vec2(-0.149693, 0.605762), vec2(0.034211, 0.979980), vec2(0.503098, -0.308878),
    vec2(-0.016205, -0.872921), vec2(0.385784, -0.393902), vec2(-0.146886, -0.859249),
    vec2(0.643361, 0.164098), vec2(0.634388, -0.049471), vec2(-0.688894, 0.007843),
    vec2(0.464034, -0.188818), vec2(-0.440840, 0.137486), vec2(0.364483, 0.511704),
    vec2(0.034028, 0.325968), vec2(0.099094, -0.308023), vec2(0.693960, -0.366253),
    vec2(0.678884, -0.204688), vec2(0.001801, 0.780328), vec2(0.145177, -0.898984),
    vec2(0.062655, -0.611866), vec2(0.315226, -0.604297), vec2(-0.780145, 0.486251),
    vec2(-0.371868, 0.882138), vec2(0.200476, 0.494430), vec2(-0.494552, -0.711051),
    vec2(0.612476, 0.705252), vec2(-0.578845, -0.768792), vec2(-0.772454, -0.090976),
    vec2(0.504440, 0.372295), vec2(0.155736, 0.065157), vec2(0.391522, 0.849605),
    vec2(-0.620106, -0.328104), vec2(0.789239, -0.419965), vec2(-0.545396, 0.538133),
    vec2(-0.178564, -0.596057)
);

float sampleCascade(int cascadeIndex, vec3 worldPos, float bias) {
    vec4 lightSpacePos = uCascadeVP[cascadeIndex] * vec4(worldPos, 1.0);
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    proj = proj * 0.5 + 0.5;
    
    // Out of bounds - return -1.0 to signal invalid
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z > 1.0)
        return -1.0;
    
    float current = proj.z - bias;
    
    // Calculate cascade pair base index (sun cascades = 0,1; moon cascades = 2,3)
    int baseCascade = (cascadeIndex / 2) * 2;
    float baseRadius = 512.0;
    float radiusScale = (cascadeIndex % 2 == 0) ? 1.0 : (uCascadeOrthoSizes[baseCascade] / uCascadeOrthoSizes[baseCascade + 1]);
    float radius = baseRadius * radiusScale * uLightTexel;
    
    float lightValue = 0.0;
    for (int i = 0; i < 64; ++i) {
        vec2 offset = POISSON[i] * radius;
        lightValue += texture(uLightMap, vec4(proj.xy + offset, cascadeIndex, current));
    }
    return lightValue / 64.0;
}

// Sample light for sun (cascades 0 and 1)
float sampleSunLight(vec3 worldPos, float bias) {
    float lightNear = sampleCascade(0, worldPos, bias);
    float lightFar = sampleCascade(1, worldPos, bias);
    
    bool nearValid = (lightNear >= 0.0);
    bool farValid = (lightFar >= 0.0);
    
    if (nearValid) {
        return lightNear;
    } else if (farValid) {
        return lightFar;
    } else {
        return 0.0;  // Dark by default
    }
}

// Sample light for moon (cascades 2 and 3)
float sampleMoonLight(vec3 worldPos, float bias) {
    float lightNear = sampleCascade(2, worldPos, bias);
    float lightFar = sampleCascade(3, worldPos, bias);
    
    bool nearValid = (lightNear >= 0.0);
    bool farValid = (lightFar >= 0.0);
    
    if (nearValid) {
        return lightNear;
    } else if (farValid) {
        return lightFar;
    } else {
        return 0.0;  // Dark by default
    }
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
        discard;
    }
    
    vec3 N = normalize(normal);
    
    // Sample sun light
    vec3 L_sun = normalize(-uSunDir);
    float ndotl_sun = max(dot(N, L_sun), 0.0);
    float bias_sun = max(0.0005, 0.001 * (1.0 - ndotl_sun));
    float sunLightFactor = sampleSunLight(worldPos, bias_sun);
    
    // Sample moon light
    vec3 L_moon = normalize(-uMoonDir);
    float ndotl_moon = max(dot(N, L_moon), 0.0);
    float bias_moon = max(0.0005, 0.001 * (1.0 - ndotl_moon));
    float moonLightFactor = sampleMoonLight(worldPos, bias_moon);
    
    // Combine sun and moon lighting (additive, moon is much dimmer)
    vec3 sunContribution = albedo * sunLightFactor * uSunIntensity;
    vec3 moonContribution = albedo * moonLightFactor * uMoonIntensity * 0.15;  // Moon is 15% as bright
    
    // Final color: dark by default, lit only where light maps indicate
    vec3 finalColor = sunContribution + moonContribution;
    
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
    // Sun cascades
    m_cascadeSplits[0] = 128.0f;    // Sun near
    m_cascadeSplits[1] = 1000.0f;   // Sun far
    // Moon cascades
    m_cascadeSplits[2] = 128.0f;    // Moon near
    m_cascadeSplits[3] = 1000.0f;   // Moon far
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
    m_loc_lightMap = glGetUniformLocation(m_shader, "uLightMap");
    m_loc_sunDir = glGetUniformLocation(m_shader, "uSunDir");
    m_loc_moonDir = glGetUniformLocation(m_shader, "uMoonDir");
    m_loc_sunIntensity = glGetUniformLocation(m_shader, "uSunIntensity");
    m_loc_moonIntensity = glGetUniformLocation(m_shader, "uMoonIntensity");
    m_loc_cameraPos = glGetUniformLocation(m_shader, "uCameraPos");
    m_loc_numCascades = glGetUniformLocation(m_shader, "uNumCascades");
    m_loc_lightTexel = glGetUniformLocation(m_shader, "uLightTexel");
    m_loc_cascadeOrthoSizes = glGetUniformLocation(m_shader, "uCascadeOrthoSizes");
    m_loc_ditherStrength = glGetUniformLocation(m_shader, "uDitherStrength");
    
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

void DeferredLightingPass::setCascadeData(int index, const glm::mat4& viewProj, float splitDistance, float orthoSize) {
    if (index >= 0 && index < 4) {
        m_cascadeVP[index] = viewProj;
        m_cascadeSplits[index] = splitDistance;
        m_cascadeOrthoSizes[index] = orthoSize;
    }
}

void DeferredLightingPass::render(const glm::vec3& sunDirection, const glm::vec3& moonDirection,
                                  float sunIntensity, float moonIntensity, const glm::vec3& cameraPosition) {
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
    
    // Bind light map (4 cascades: sun near, sun far, moon near, moon far)
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_shadowMap.getDepthTexture());
    if (m_loc_lightMap >= 0) glUniform1i(m_loc_lightMap, 5);
    
    // Set lighting uniforms
    if (m_loc_sunDir >= 0) glUniform3fv(m_loc_sunDir, 1, &sunDirection[0]);
    if (m_loc_moonDir >= 0) glUniform3fv(m_loc_moonDir, 1, &moonDirection[0]);
    if (m_loc_sunIntensity >= 0) glUniform1f(m_loc_sunIntensity, sunIntensity);
    if (m_loc_moonIntensity >= 0) glUniform1f(m_loc_moonIntensity, moonIntensity);
    if (m_loc_cameraPos >= 0) glUniform3fv(m_loc_cameraPos, 1, &cameraPosition[0]);
    if (m_loc_numCascades >= 0) glUniform1i(m_loc_numCascades, g_shadowMap.getNumCascades());
    
    float lightTexel = 1.0f / static_cast<float>(g_shadowMap.getSize());
    if (m_loc_lightTexel >= 0) glUniform1f(m_loc_lightTexel, lightTexel);
    
    // Set cascade matrices (4 cascades)
    for (int i = 0; i < 4; ++i) {
        std::string uniformName = "uCascadeVP[" + std::to_string(i) + "]";
        GLint loc = glGetUniformLocation(m_shader, uniformName.c_str());
        if (loc >= 0) {
            glUniformMatrix4fv(loc, 1, GL_FALSE, &m_cascadeVP[i][0][0]);
        }
    }
    
    // Set cascade ortho sizes for proper PCF radius scaling (4 cascades)
    if (m_loc_cascadeOrthoSizes >= 0) {
        glUniform1fv(m_loc_cascadeOrthoSizes, 4, m_cascadeOrthoSizes);
    }
    
    // Set dither strength for shadow sampling
    if (m_loc_ditherStrength >= 0) {
        glUniform1f(m_loc_ditherStrength, m_ditherStrength);
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
