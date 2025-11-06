#include "SunRenderer.h"
#include "Parameters.h"
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

// Global sun renderer instance
SunRenderer g_sunRenderer;

namespace {

const char* vertexShader = R"(
#version 460 core

layout (location = 0) in vec2 aPos;

uniform vec2 uSunScreenPos;     // Sun position in screen space (-1 to 1)
uniform float uSunSize;         // Size of the sun disc
uniform vec2 uScreenSize;       // Screen dimensions

out vec2 vUV;                   // UV coordinates for the quad
out vec2 vSunUV;                // UV relative to sun center

void main() {
    // Calculate quad position around sun
    vec2 quadPos = uSunScreenPos + aPos * uSunSize;
    
    // Output position
    gl_Position = vec4(quadPos, 0.0, 1.0);
    
    // UV coordinates for texture sampling
    vUV = aPos * 0.5 + 0.5;
    
    // UV relative to sun center (for distance calculation)
    vSunUV = aPos;
}
)";

const char* fragmentShader = R"(
#version 460 core

in vec2 vUV;
in vec2 vSunUV;

uniform vec3 uSunColor;
uniform float uSunIntensity;

out vec4 FragColor;

void main() {
    // Distance from center of sun disc
    float distFromCenter = length(vSunUV);
    
    // Create sun disc with soft falloff
    float sunDisc = 1.0 - smoothstep(0.3, 1.0, distFromCenter);
    
    // Create sun glow (wider, softer)
    float sunGlow = 1.0 - smoothstep(0.0, 1.2, distFromCenter);
    sunGlow = pow(sunGlow, 3.0) * 0.3; // Softer glow
    
    // Combine disc and glow
    float sunMask = sunDisc + sunGlow;
    
    // Apply sun color and intensity
    vec3 finalColor = uSunColor * sunMask * uSunIntensity;
    
    // Use additive blending - alpha controls contribution
    FragColor = vec4(finalColor, sunMask);
}
)";

// Helper function to compile shader
GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "❌ Shader compilation failed: " << infoLog << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

// Helper function to link program
GLuint linkProgram(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        std::cerr << "❌ Shader linking failed: " << infoLog << std::endl;
        glDeleteProgram(program);
        return 0;
    }
    
    return program;
}

} // anonymous namespace

SunRenderer::SunRenderer() = default;
SunRenderer::~SunRenderer() {
    shutdown();
}

bool SunRenderer::initialize() {
    if (m_initialized) return true;
    
    std::cout << "🌞 Initializing Sun Renderer..." << std::endl;
    
    if (!createShader()) {
        std::cerr << "❌ Failed to create sun shader!" << std::endl;
        return false;
    }
    
    if (!createGeometry()) {
        std::cerr << "❌ Failed to create sun geometry!" << std::endl;
        shutdown();
        return false;
    }
    
    m_initialized = true;
    std::cout << "✅ Sun Renderer initialized!" << std::endl;
    return true;
}

void SunRenderer::shutdown() {
    deleteShader();
    
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

void SunRenderer::render(const glm::vec3& sunDirection, 
                        const glm::mat4& cameraView, 
                        const glm::mat4& cameraProjection,
                        int screenWidth, 
                        int screenHeight) {
    if (!m_initialized) return;
    
    // Calculate sun direction in screen space - sun is at infinite distance
    // Transform just the direction, not a position
    glm::vec4 sunDirView = cameraView * glm::vec4(-sunDirection, 0.0f); // w=0 for direction
    glm::vec4 sunDirClip = cameraProjection * glm::vec4(sunDirView.xyz, 1.0f);
    
    // Check if sun direction is behind camera
    if (sunDirClip.w <= 0.0f) return;
    
    // Convert to normalized device coordinates
    glm::vec3 sunNDC = glm::vec3(sunDirClip) / sunDirClip.w;
    
    // Check if sun is outside screen bounds (with some margin)
    if (sunNDC.x < -1.5f || sunNDC.x > 1.5f || 
        sunNDC.y < -1.5f || sunNDC.y > 1.5f) {
        return;
    }
    
    // Sun is always the same color - like real life
    glm::vec3 sunColor = glm::vec3(1.0f, 0.95f, 0.8f);  // Consistent warm white
    
    // Enable additive blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE); // Don't write to depth buffer
    
    // Use sun shader
    glUseProgram(m_shader);
    
    // Set uniforms
    if (m_loc_sunScreenPos >= 0)
        glUniform2f(m_loc_sunScreenPos, sunNDC.x, sunNDC.y);
    if (m_loc_sunSize >= 0)
        glUniform1f(m_loc_sunSize, m_sunSize);
    if (m_loc_sunColor >= 0)
        glUniform3fv(m_loc_sunColor, 1, &sunColor[0]);
    if (m_loc_sunIntensity >= 0)
        glUniform1f(m_loc_sunIntensity, m_sunIntensity);
    if (m_loc_screenSize >= 0)
        glUniform2f(m_loc_screenSize, (float)screenWidth, (float)screenHeight);
    
    // Render quad
    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    
    // Restore render state
    glUseProgram(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

bool SunRenderer::createShader() {
    // Compile shaders
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexShader);
    if (!vs) return false;
    
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentShader);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    
    // Link program
    m_shader = linkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    if (!m_shader) return false;
    
    // Cache uniform locations
    m_loc_sunScreenPos = glGetUniformLocation(m_shader, "uSunScreenPos");
    m_loc_sunSize = glGetUniformLocation(m_shader, "uSunSize");
    m_loc_sunColor = glGetUniformLocation(m_shader, "uSunColor");
    m_loc_sunIntensity = glGetUniformLocation(m_shader, "uSunIntensity");
    m_loc_screenSize = glGetUniformLocation(m_shader, "uScreenSize");
    
    return true;
}

bool SunRenderer::createGeometry() {
    // Create fullscreen quad vertices (position only)
    float vertices[] = {
        // Triangle 1
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        // Triangle 2
        -1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f
    };
    
    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &m_quadVBO);
    
    glBindVertexArray(m_quadVAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glBindVertexArray(0);
    
    return true;
}

void SunRenderer::deleteShader() {
    if (m_shader) {
        glDeleteProgram(m_shader);
        m_shader = 0;
    }
}