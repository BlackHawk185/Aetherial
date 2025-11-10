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

    // Tone mapping fragment shader
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
    // Create final texture (LDR format for output)
    glGenTextures(1, &m_finalTexture);
    glBindTexture(GL_TEXTURE_2D, m_finalTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
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
    // Compile vertex shader
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kQuadVS);
    if (!vs) return false;
    
    // Compile tone mapping fragment shader
    GLuint toneMappingFS = CompileShader(GL_FRAGMENT_SHADER, kToneMappingFS);
    if (!toneMappingFS) {
        glDeleteShader(vs);
        return false;
    }
    
    // Link tone mapping shader
    m_toneMappingShader = LinkProgram(vs, toneMappingFS);
    if (!m_toneMappingShader) {
        glDeleteShader(vs);
        glDeleteShader(toneMappingFS);
        return false;
    }
    
    // Clean up shaders
    glDeleteShader(vs);
    glDeleteShader(toneMappingFS);
    
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
    if (m_finalTexture) {
        glDeleteTextures(1, &m_finalTexture);
        m_finalTexture = 0;
    }
    
    if (m_finalFBO) {
        glDeleteFramebuffers(1, &m_finalFBO);
        m_finalFBO = 0;
    }
}

void PostProcessingPipeline::deleteShaders() {
    if (m_toneMappingShader) {
        glDeleteProgram(m_toneMappingShader);
        m_toneMappingShader = 0;
    }
}

void PostProcessingPipeline::process(GLuint inputTexture, GLuint depthTexture, const glm::vec3& sunDirection, 
                                     const glm::vec3& cameraPosition, const glm::mat4& viewProjectionMatrix) {
    if (!m_initialized || !m_enabled) return;
    
    // Unused parameters (kept for API compatibility)
    (void)depthTexture;
    (void)sunDirection;
    (void)cameraPosition;
    (void)viewProjectionMatrix;
    
    if (inputTexture == 0) {
        std::cerr << "❌ PostProcessingPipeline: No input texture provided!" << std::endl;
        return;
    }
    
    // Tone mapping: HDR -> LDR
    if (EngineParameters::PostProcessing::ENABLE_TONE_MAPPING) {
        renderToneMapping(inputTexture);
    } else {
        // Skip tone mapping - copy input directly to final
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_finalFBO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, inputTexture);
        // Copy without tone mapping - just blit the HDR texture directly
        glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    
    // 3. Copy final result back to screen
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_finalFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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