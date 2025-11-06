#include "SkyRenderer.h"
#include <glad/gl.h>
#include <iostream>

// Global sky renderer instance
SkyRenderer g_skyRenderer;

namespace {
    // Fullscreen quad vertex shader
    static const char* kVS = R"GLSL(
#version 460 core
layout(location = 0) in vec2 aPos;

out vec2 vUV;
out vec3 vRayDir;

uniform float uAspectRatio;
uniform mat4 uViewMatrix;

void main() {
    vUV = aPos * 0.5 + 0.5;
    
    // Create ray direction for atmospheric calculation
    // Convert screen space to world space ray direction using inverse view matrix
    vec3 screenPos = vec3(aPos.x * uAspectRatio, aPos.y, -1.0);
    
    // Transform to world space using inverse view matrix (no translation)
    mat3 invViewRot = transpose(mat3(uViewMatrix));
    vRayDir = normalize(invViewRot * screenPos);
    
    // Output vertex at far plane (z = 1.0 in NDC)
    gl_Position = vec4(aPos, 1.0, 1.0);
}
)GLSL";

    // Sky fragment shader with sun disc
    static const char* kFS = R"GLSL(
#version 460 core
in vec2 vUV;
in vec3 vRayDir;

uniform vec3 uSunDir;
uniform float uSunIntensity;
uniform vec3 uCameraPos;
uniform float uSunSize;
uniform float uSunGlow;
uniform float uExposure;

out vec4 FragColor;

// Calculate sky gradient (same as deferred lighting pass)
vec3 calculateSkyGradient(vec2 uv, vec3 sunDir) {
    // Convert UV to normalized device coordinates for gradient
    float height = (uv.y - 0.5) * 2.0;  // -1 to 1, with 1 at top
    
    // Sun height determines time of day
    float sunHeight = -sunDir.y;
    
    // Day colors - more vivid blue sky
    vec3 daySky = vec3(0.3, 0.5, 0.8);        // Deeper, more saturated blue
    vec3 dayHorizon = vec3(0.6, 0.7, 0.85);   // Lighter blue horizon
    
    // Night colors
    vec3 nightSky = vec3(0.02, 0.02, 0.05);
    vec3 nightHorizon = vec3(0.05, 0.05, 0.1);
    
    // Sunset/sunrise colors
    vec3 sunsetSky = vec3(0.3, 0.2, 0.4);
    vec3 sunsetHorizon = vec3(1.0, 0.5, 0.2);
    
    vec3 skyColor, horizonColor;
    
    // Smooth transitions throughout the day using smoothstep
    if (sunHeight < -0.5) {
        // Deep night
        skyColor = nightSky;
        horizonColor = nightHorizon;
    } else if (sunHeight < -0.1) {
        // Night to sunset transition
        float t = smoothstep(-0.5, -0.1, sunHeight);
        skyColor = mix(nightSky, sunsetSky, t);
        horizonColor = mix(nightHorizon, sunsetHorizon, t);
    } else if (sunHeight < 0.2) {
        // Sunset/sunrise period
        skyColor = sunsetSky;
        horizonColor = sunsetHorizon;
    } else if (sunHeight < 0.5) {
        // Sunrise to day transition
        float t = smoothstep(0.2, 0.5, sunHeight);
        skyColor = mix(sunsetSky, daySky, t);
        horizonColor = mix(sunsetHorizon, dayHorizon, t);
    } else {
        // Full day
        skyColor = daySky;
        horizonColor = dayHorizon;
    }
    
    // Vertical gradient
    float gradientT = smoothstep(-0.5, 0.8, height);
    return mix(horizonColor, skyColor, gradientT);
}

// Calculate sun disc and glow
vec3 calculateSunDisc(vec3 rayDir, vec3 sunDir, float sunIntensity) {
    // Distance from ray to sun direction
    float sunDot = dot(rayDir, -sunDir);
    float sunDistance = length(rayDir - (-sunDir * sunDot));
    
    // Sun disc (sharp falloff)
    float sunDisc = 1.0 - smoothstep(0.0, uSunSize, sunDistance);
    
    // Sun glow (wider, softer falloff)
    float sunGlow = 1.0 - smoothstep(0.0, uSunSize * uSunGlow, sunDistance);
    sunGlow = pow(sunGlow, 2.0);
    
    // Sun is always the same color - like real life
    vec3 sunColor = vec3(1.0, 0.95, 0.8);  // Consistent warm white
    
    // Combine disc and glow
    vec3 sun = sunColor * (sunDisc * 10.0 + sunGlow * 0.3) * sunIntensity;
    
    return sun;
}

// Generate procedural stars using screen UV coordinates (camera-independent)
vec3 calculateStars(vec3 rayDir) {
    vec3 starColor = vec3(0.3, 0.5, 0.8);  // Same as day sky color
    
    // Use UV coordinates instead of ray direction for camera-independent stars
    // Convert ray to spherical coordinates for stable star positions
    float phi = atan(rayDir.z, rayDir.x);        // Azimuth angle
    float theta = asin(rayDir.y);                // Elevation angle
    vec2 sphericalUV = vec2(phi / 6.28318, theta / 3.14159 + 0.5);
    
    float stars = 0.0;
    
    // Small sharp stars - no glow, just points of light
    vec2 grid = sphericalUV * 25.0;  // Much finer grid = smaller stars
    vec2 cellCenter = floor(grid) + 0.5;
    float hash = fract(sin(dot(cellCenter, vec2(12.9898, 78.233))) * 43758.5453);
    
    if (hash > 0.92) {  // 8% chance per cell - fewer stars
        float dist = length(grid - cellCenter);
        float starSize = 0.08;  // Much smaller, sharp stars
        if (dist < starSize) {
            float intensity = 1.0 - (dist / starSize);  // Sharp falloff, not smooth
            stars += intensity * 1.0;
        }
    }
    
    return starColor * stars;
}

void main() {
    // Base sky gradient
    vec3 skyColor = calculateSkyGradient(vUV, uSunDir);
    
    // Add stars (always present, hidden by sky brightness during day)
    vec3 starContribution = calculateStars(vRayDir);
    
    // Add sun disc and glow
    vec3 sunContribution = calculateSunDisc(vRayDir, uSunDir, uSunIntensity);
    
    // Combine sky, stars, and sun with controlled blending
    // Stars are added first so sky brightness naturally hides them during day
    vec3 finalColor = skyColor + starContribution + sunContribution * 0.8;
    
    // Apply exposure (but clamp to prevent washout)
    finalColor *= uExposure;
    finalColor = min(finalColor, vec3(1.2));  // Prevent excessive brightening
    
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
            std::cerr << "❌ Sky shader compilation failed:\n" << log << std::endl;
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
            std::cerr << "❌ Sky shader linking failed:\n" << log << std::endl;
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }
}

SkyRenderer::SkyRenderer() {
    // Constructor
}

SkyRenderer::~SkyRenderer() {
    shutdown();
}

bool SkyRenderer::initialize() {
    if (m_initialized) return true;
    
    std::cout << "🌅 Initializing Sky Renderer..." << std::endl;
    
    if (!createShaders()) {
        std::cerr << "❌ Failed to create sky shaders" << std::endl;
        return false;
    }
    
    if (!createGeometry()) {
        std::cerr << "❌ Failed to create sky geometry" << std::endl;
        return false;
    }
    
    m_initialized = true;
    std::cout << "   └─ Sky renderer initialized successfully" << std::endl;
    return true;
}

void SkyRenderer::shutdown() {
    if (m_shader) {
        glDeleteProgram(m_shader);
        m_shader = 0;
    }
    
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

bool SkyRenderer::createShaders() {
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
    m_loc_sunDir = glGetUniformLocation(m_shader, "uSunDir");
    m_loc_sunIntensity = glGetUniformLocation(m_shader, "uSunIntensity");
    m_loc_cameraPos = glGetUniformLocation(m_shader, "uCameraPos");
    m_loc_sunSize = glGetUniformLocation(m_shader, "uSunSize");
    m_loc_sunGlow = glGetUniformLocation(m_shader, "uSunGlow");
    m_loc_exposure = glGetUniformLocation(m_shader, "uExposure");
    m_loc_aspectRatio = glGetUniformLocation(m_shader, "uAspectRatio");
    m_loc_viewMatrix = glGetUniformLocation(m_shader, "uViewMatrix");
    
    return true;
}

bool SkyRenderer::createGeometry() {
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

void SkyRenderer::render(const glm::vec3& sunDirection, float sunIntensity, const glm::vec3& cameraPosition,
                        const glm::mat4& viewMatrix, float aspectRatio) {
    if (!m_initialized) return;
    
    // Use sky shader
    glUseProgram(m_shader);
    
    // Update uniforms
    updateUniforms(sunDirection, sunIntensity, cameraPosition, viewMatrix, aspectRatio);
    
    // Set render state for sky (render at far plane, replace background pixels)
    glDepthMask(GL_FALSE);        // Don't write to depth buffer
    glDepthFunc(GL_LEQUAL);       // Accept pixels at far plane (depth = 1.0)
    glEnable(GL_DEPTH_TEST);      // Use depth test to only render sky pixels
    glDisable(GL_BLEND);          // Replace the dark background completely
    
    // Render fullscreen quad
    glBindVertexArray(m_quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    
    // Restore render state
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);         // Restore normal depth testing
    
    glUseProgram(0);
}

void SkyRenderer::updateUniforms(const glm::vec3& sunDirection, float sunIntensity, const glm::vec3& cameraPosition,
                                 const glm::mat4& viewMatrix, float aspectRatio) {
    if (m_loc_sunDir >= 0)
        glUniform3fv(m_loc_sunDir, 1, &sunDirection[0]);
    
    if (m_loc_sunIntensity >= 0)
        glUniform1f(m_loc_sunIntensity, sunIntensity);
    
    if (m_loc_cameraPos >= 0)
        glUniform3fv(m_loc_cameraPos, 1, &cameraPosition[0]);
    
    if (m_loc_sunSize >= 0)
        glUniform1f(m_loc_sunSize, m_sunSize);
    
    if (m_loc_sunGlow >= 0)
        glUniform1f(m_loc_sunGlow, m_sunGlow);
    
    if (m_loc_exposure >= 0)
        glUniform1f(m_loc_exposure, m_exposure);
    
    if (m_loc_aspectRatio >= 0)
        glUniform1f(m_loc_aspectRatio, aspectRatio);
    
    if (m_loc_viewMatrix >= 0)
        glUniformMatrix4fv(m_loc_viewMatrix, 1, GL_FALSE, &viewMatrix[0][0]);
}