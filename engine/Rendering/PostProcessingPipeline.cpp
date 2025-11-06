#include "PostProcessingPipeline.h"
#include "Parameters.h"  // Add this include
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

// Global post-processing pipeline instance
PostProcessingPipeline g_postProcessing;

namespace {
    // Fullscreen quad vertex shader (shared by all post-processing effects)
    static const char* kQuadVS = R"GLSL(
#version 460 core
layout(location = 0) in vec2 aPos;

out vec2 vUV;

void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

    // Godray/volumetric lighting fragment shader - dynamically generated with parameters
    std::string godrayFS = R"GLSL(
#version 460 core
in vec2 vUV;

uniform sampler2D uInputTexture;    // HDR scene texture
uniform sampler2D uDepthTexture;    // Scene depth buffer
uniform vec3 uSunDirection;         // Direction TO the sun
uniform vec3 uCameraPosition;       // Camera position
uniform float uIntensity;           // Godray intensity
uniform float uDecay;               // Light decay factor
uniform float uDensity;             // Sampling density
uniform float uWeight;              // Light weight
uniform mat4 uViewProjectionMatrix; // For accurate sun screen position

out vec4 FragColor;

const int NUM_SAMPLES = )GLSL" + std::to_string(EngineParameters::PostProcessing::GODRAY_SAMPLES) + R"GLSL(;

vec2 worldToScreen(vec3 worldPos) {
    // Transform world position to clip space
    vec4 clipPos = uViewProjectionMatrix * vec4(worldPos, 1.0);
    
    // Perspective divide to get NDC
    vec3 ndc = clipPos.xyz / clipPos.w;
    
    // Convert NDC to screen UV coordinates (0 to 1)
    return ndc.xy * 0.5 + 0.5;
}

void main() {
    // Sample the original scene color
    vec3 sceneColor = texture(uInputTexture, vUV).rgb;
    float depth = texture(uDepthTexture, vUV).r;
    
    // Calculate sun position in screen space using proper projection
    // Sun is at infinite distance - position independent of camera location
    vec3 sunWorldPos = -uSunDirection * 100000.0;  // Very far away, no camera dependency
    vec2 sunScreenPos = worldToScreen(sunWorldPos);
    
    // Vector from current pixel to sun
    vec2 deltaTexCoord = (sunScreenPos - vUV);
    deltaTexCoord *= 1.0 / float(NUM_SAMPLES) * uDensity;
    
    // Initial sample position
    vec2 samplePos = vUV;
    
    // Accumulate light samples along ray toward sun
    float illuminationDecay = 1.0;
    vec3 godrayColor = vec3(0.0);
    
    for (int i = 0; i < NUM_SAMPLES; i++) {
        samplePos += deltaTexCoord;
        
        // Sample depth at this position
        float sampleDepth = texture(uDepthTexture, samplePos).r;
        
        // Smooth occlusion with gentler falloff
        float occlusionFactor = smoothstep(0.9, 1.0, sampleDepth);
        
        // Generate smooth light contribution
        vec3 sampleColor = vec3(1.0);
        sampleColor *= illuminationDecay * uWeight * occlusionFactor;
        
        godrayColor += sampleColor;
        illuminationDecay *= uDecay;
    }
    
    // Sun is always the same color - like real life
    vec3 sunColor = vec3(1.0, 0.95, 0.8);  // Consistent warm white
    
    // Apply sun color and intensity
    godrayColor *= sunColor * uIntensity;
    
    // Combine with original scene
    vec3 finalColor = sceneColor + godrayColor;
    
    FragColor = vec4(finalColor, 1.0);
}
)GLSL";

    // Tone mapping fragment shader (based on existing postprocess.frag)
    static const char* kToneMappingFS = R"GLSL(
#version 460 core
in vec2 vUV;

uniform sampler2D uHDRTexture;
uniform float uExposure;
uniform float uGamma;

out vec4 FragColor;

// ACES tone mapping
vec3 acesToneMapping(vec3 color, float exposure) {
    color *= exposure;
    
    // ACES tone mapping curve fit
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 gammaCorrection(vec3 color, float gamma) {
    return pow(color, vec3(1.0 / gamma));
}

void main() {
    // Sample HDR color
    vec3 hdrColor = texture(uHDRTexture, vUV).rgb;
    
    // Tone mapping
    vec3 ldrColor = acesToneMapping(hdrColor, uExposure);
    
    // Gamma correction
    ldrColor = gammaCorrection(ldrColor, uGamma);
    
    FragColor = vec4(ldrColor, 1.0);
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
            std::cerr << "❌ Post-processing shader compilation failed:\n" << log << std::endl;
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
            std::cerr << "❌ Post-processing shader linking failed:\n" << log << std::endl;
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }
}

PostProcessingPipeline::PostProcessingPipeline() {
    // Constructor
}

PostProcessingPipeline::~PostProcessingPipeline() {
    shutdown();
}

bool PostProcessingPipeline::initialize(int width, int height) {
    if (m_initialized) {
        shutdown();
    }
    
    m_width = width;
    m_height = height;
    
    std::cout << "🌈 Initializing Post-Processing Pipeline (" << width << "x" << height << ")..." << std::endl;
    
    if (!createFramebuffers()) {
        std::cerr << "❌ Failed to create post-processing framebuffers" << std::endl;
        return false;
    }
    
    if (!createShaders()) {
        std::cerr << "❌ Failed to create post-processing shaders" << std::endl;
        return false;
    }
    
    if (!createGeometry()) {
        std::cerr << "❌ Failed to create post-processing geometry" << std::endl;
        return false;
    }
    
    m_initialized = true;
    std::cout << "   └─ Post-processing pipeline initialized successfully" << std::endl;
    return true;
}

void PostProcessingPipeline::shutdown() {
    deleteFramebuffers();
    deleteShaders();
    
    if (m_quadVAO) {
        glDeleteVertexArrays(1, &m_quadVAO);
        m_quadVAO = 0;
    }
    
    if (m_quadVBO) {
        glDeleteBuffers(1, &m_quadVBO);
        m_quadVBO = 0;
    }
    
    m_initialized = false;
}

bool PostProcessingPipeline::resize(int width, int height) {
    if (width == m_width && height == m_height) {
        return true;  // No change needed
    }
    
    m_width = width;
    m_height = height;
    
    deleteFramebuffers();
    return createFramebuffers();
}

bool PostProcessingPipeline::createFramebuffers() {
    // Create intermediate texture (HDR format for effects processing)
    glGenTextures(1, &m_intermediateTexture);
    glBindTexture(GL_TEXTURE_2D, m_intermediateTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Create final texture (LDR format for output)
    glGenTextures(1, &m_finalTexture);
    glBindTexture(GL_TEXTURE_2D, m_finalTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Create intermediate framebuffer
    glGenFramebuffers(1, &m_intermediateFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_intermediateFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_intermediateTexture, 0);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "❌ Intermediate framebuffer not complete!" << std::endl;
        return false;
    }
    
    // Create final framebuffer
    glGenFramebuffers(1, &m_finalFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_finalFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_finalTexture, 0);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "❌ Final framebuffer not complete!" << std::endl;
        return false;
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool PostProcessingPipeline::createShaders() {
    // Compile vertex shader (shared)
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kQuadVS);
    if (!vs) return false;
    
    // Compile godray fragment shader
    GLuint godrayFSShader = CompileShader(GL_FRAGMENT_SHADER, godrayFS.c_str());
    if (!godrayFSShader) {
        glDeleteShader(vs);
        return false;
    }
    
    // Compile tone mapping fragment shader
    GLuint toneMappingFS = CompileShader(GL_FRAGMENT_SHADER, kToneMappingFS);
    if (!toneMappingFS) {
        glDeleteShader(vs);
        glDeleteShader(godrayFSShader);
        return false;
    }
    
    // Link godray shader
    m_godrayShader = LinkProgram(vs, godrayFSShader);
    if (!m_godrayShader) {
        glDeleteShader(vs);
        glDeleteShader(godrayFSShader);
        glDeleteShader(toneMappingFS);
        return false;
    }
    
    // Link tone mapping shader
    m_toneMappingShader = LinkProgram(vs, toneMappingFS);
    if (!m_toneMappingShader) {
        glDeleteShader(vs);
        glDeleteShader(godrayFSShader);
        glDeleteShader(toneMappingFS);
        return false;
    }
    
    // Clean up fragment shaders
    glDeleteShader(vs);
    glDeleteShader(godrayFSShader);
    glDeleteShader(toneMappingFS);
    
    // Cache uniform locations for godray shader
    m_godray_loc_inputTexture = glGetUniformLocation(m_godrayShader, "uInputTexture");
    m_godray_loc_depthTexture = glGetUniformLocation(m_godrayShader, "uDepthTexture");
    m_godray_loc_sunDirection = glGetUniformLocation(m_godrayShader, "uSunDirection");
    m_godray_loc_cameraPosition = glGetUniformLocation(m_godrayShader, "uCameraPosition");
    m_godray_loc_intensity = glGetUniformLocation(m_godrayShader, "uIntensity");
    m_godray_loc_decay = glGetUniformLocation(m_godrayShader, "uDecay");
    m_godray_loc_density = glGetUniformLocation(m_godrayShader, "uDensity");
    m_godray_loc_weight = glGetUniformLocation(m_godrayShader, "uWeight");
    m_godray_loc_viewProjectionMatrix = glGetUniformLocation(m_godrayShader, "uViewProjectionMatrix");
    
    // Cache uniform locations for tone mapping shader
    m_tone_loc_hdrTexture = glGetUniformLocation(m_toneMappingShader, "uHDRTexture");
    m_tone_loc_exposure = glGetUniformLocation(m_toneMappingShader, "uExposure");
    m_tone_loc_gamma = glGetUniformLocation(m_toneMappingShader, "uGamma");
    
    return true;
}

bool PostProcessingPipeline::createGeometry() {
    // Fullscreen quad vertices
    float quadVertices[] = {
        // positions
        -1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f
    };
    
    unsigned int indices[] = {
        0, 1, 2,
        0, 2, 3
    };
    
    // Create VAO
    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &m_quadVBO);
    GLuint quadEBO;
    glGenBuffers(1, &quadEBO);
    
    glBindVertexArray(m_quadVAO);
    
    // Vertex buffer
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    
    // Index buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glBindVertexArray(0);
    
    return true;
}

void PostProcessingPipeline::deleteFramebuffers() {
    if (m_intermediateTexture) {
        glDeleteTextures(1, &m_intermediateTexture);
        m_intermediateTexture = 0;
    }
    
    if (m_finalTexture) {
        glDeleteTextures(1, &m_finalTexture);
        m_finalTexture = 0;
    }
    
    if (m_intermediateFBO) {
        glDeleteFramebuffers(1, &m_intermediateFBO);
        m_intermediateFBO = 0;
    }
    
    if (m_finalFBO) {
        glDeleteFramebuffers(1, &m_finalFBO);
        m_finalFBO = 0;
    }
}

void PostProcessingPipeline::deleteShaders() {
    if (m_godrayShader) {
        glDeleteProgram(m_godrayShader);
        m_godrayShader = 0;
    }
    
    if (m_toneMappingShader) {
        glDeleteProgram(m_toneMappingShader);
        m_toneMappingShader = 0;
    }
}

void PostProcessingPipeline::process(GLuint inputTexture, GLuint depthTexture, const glm::vec3& sunDirection, 
                                     const glm::vec3& cameraPosition, const glm::mat4& viewProjectionMatrix) {
    if (!m_initialized || !m_enabled) return;
    
    // We should always have a valid HDR input texture now
    if (inputTexture == 0) {
        std::cerr << "❌ PostProcessingPipeline: No input texture provided!" << std::endl;
        return;
    }
    
    // 1. Godray pass: inputTexture -> intermediateTexture (if enabled)
    if (EngineParameters::PostProcessing::ENABLE_GODRAYS) {
        renderGodrays(inputTexture, depthTexture, sunDirection, cameraPosition, viewProjectionMatrix);
    } else {
        // Skip godrays - copy input directly to intermediate
        glBindFramebuffer(GL_FRAMEBUFFER, m_intermediateFBO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, inputTexture);
        glUseProgram(0); // Use fixed function for simple copy
        // Simple fullscreen blit would go here, but we'll use tone mapping step
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    
    // 2. Tone mapping pass: intermediateTexture -> finalTexture (if enabled)
    if (EngineParameters::PostProcessing::ENABLE_TONE_MAPPING) {
        GLuint sourceTexture = EngineParameters::PostProcessing::ENABLE_GODRAYS ? m_intermediateTexture : inputTexture;
        renderToneMapping(sourceTexture);
    } else {
        // Skip tone mapping - copy source directly to final
        GLuint sourceTexture = EngineParameters::PostProcessing::ENABLE_GODRAYS ? m_intermediateTexture : inputTexture;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_finalFBO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        // Copy without tone mapping - just blit the HDR texture directly
        glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    
    // 3. Copy final result back to screen
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_finalFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PostProcessingPipeline::renderGodrays(GLuint inputTexture, GLuint depthTexture, const glm::vec3& sunDirection, 
                                          const glm::vec3& cameraPosition, const glm::mat4& viewProjectionMatrix) {
    // Bind intermediate framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, m_intermediateFBO);
    glViewport(0, 0, m_width, m_height);
    
    // Use godray shader
    glUseProgram(m_godrayShader);
    
    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depthTexture);
    
    // Set uniforms
    if (m_godray_loc_inputTexture >= 0)
        glUniform1i(m_godray_loc_inputTexture, 0);
    if (m_godray_loc_depthTexture >= 0)
        glUniform1i(m_godray_loc_depthTexture, 1);
    if (m_godray_loc_sunDirection >= 0)
        glUniform3fv(m_godray_loc_sunDirection, 1, &sunDirection[0]);
    if (m_godray_loc_cameraPosition >= 0)
        glUniform3fv(m_godray_loc_cameraPosition, 1, &cameraPosition[0]);
    if (m_godray_loc_viewProjectionMatrix >= 0)
        glUniformMatrix4fv(m_godray_loc_viewProjectionMatrix, 1, GL_FALSE, &viewProjectionMatrix[0][0]);
    if (m_godray_loc_intensity >= 0)
        glUniform1f(m_godray_loc_intensity, m_godrayIntensity);
    if (m_godray_loc_decay >= 0)
        glUniform1f(m_godray_loc_decay, m_godrayDecay);
    if (m_godray_loc_density >= 0)
        glUniform1f(m_godray_loc_density, m_godrayDensity);
    if (m_godray_loc_weight >= 0)
        glUniform1f(m_godray_loc_weight, m_godrayWeight);
    
    // Render fullscreen quad
    glBindVertexArray(m_quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    
    glUseProgram(0);
}

void PostProcessingPipeline::renderToneMapping(GLuint inputTexture) {
    // Bind final framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, m_finalFBO);
    glViewport(0, 0, m_width, m_height);
    
    // Use tone mapping shader
    glUseProgram(m_toneMappingShader);
    
    // Bind texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTexture);
    
    // Set uniforms
    if (m_tone_loc_hdrTexture >= 0)
        glUniform1i(m_tone_loc_hdrTexture, 0);
    if (m_tone_loc_exposure >= 0)
        glUniform1f(m_tone_loc_exposure, m_exposure);
    if (m_tone_loc_gamma >= 0)
        glUniform1f(m_tone_loc_gamma, m_gamma);
    
    // Render fullscreen quad
    glBindVertexArray(m_quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}