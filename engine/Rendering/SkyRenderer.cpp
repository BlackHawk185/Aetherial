#include "SkyRenderer.h"
#include <glad/gl.h>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>

// Global sky renderer instance
SkyRenderer g_skyRenderer;

namespace {
    // Skybox cube vertex shader
    static const char* kVS = R"GLSL(
#version 460 core
layout(location = 0) in vec3 aPos;

out vec3 vWorldPos;

uniform mat4 uProjection;
uniform mat4 uView;

void main() {
    vWorldPos = aPos;
    
    // Remove translation from view matrix for skybox
    mat4 viewNoTranslation = mat4(mat3(uView));
    vec4 pos = uProjection * viewNoTranslation * vec4(aPos, 1.0);
    
    // Set z to w to ensure skybox is at far plane after perspective divide
    gl_Position = pos.xyww;
}
)GLSL";

    // Sky fragment shader with stars, sun, moon, and gradients
    static const char* kFS = R"GLSL(
#version 460 core
in vec3 vWorldPos;

uniform vec3 uSunDir;
uniform float uSunIntensity;
uniform vec3 uMoonDir;
uniform float uMoonIntensity;
uniform float uTimeOfDay;
uniform vec3 uCameraPos;
uniform float uSunSize;
uniform float uSunGlow;
uniform float uMoonSize;
uniform float uExposure;

out vec4 FragColor;

// Generate pseudo-random value for star positions
float hash(vec3 p) {
    p = fract(p * 0.3183099);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Generate starfield
vec3 generateStars(vec3 rayDir, vec3 skyColor) {
    // Sample star positions using noise
    vec3 p = rayDir * 100.0; // Scale for star density
    vec3 gridPos = floor(p);
    vec3 localPos = fract(p);
    
    vec3 stars = vec3(0.0);
    
    // Check 27 neighboring grid cells for stars
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            for (int z = -1; z <= 1; z++) {
                vec3 cellPos = gridPos + vec3(x, y, z);
                float h = hash(cellPos);
                
                // Only some cells have stars (make them smaller - half of current size)
                if (h > 0.999) {  // Increased threshold for fewer, smaller stars
                    // Star position within cell
                    vec3 starPos = cellPos + vec3(
                        hash(cellPos + vec3(1.0, 2.0, 3.0)),
                        hash(cellPos + vec3(4.0, 5.0, 6.0)),
                        hash(cellPos + vec3(7.0, 8.0, 9.0))
                    );
                    
                    vec3 starDir = normalize(starPos);
                    float alignment = dot(rayDir, starDir);
                    
                    // Star brightness based on alignment (half the size)
                    if (alignment > 0.9999) {  // Tighter alignment for smaller stars
                        float brightness = pow(max(0.0, (alignment - 0.9999) / 0.0001), 3.0);
                        
                        // Make stars match daytime sky color so they're invisible during day
                        vec3 dayTimeSkyColor = vec3(0.5, 0.7, 1.0);  // Match daytime sky color exactly
                        vec3 starColor = dayTimeSkyColor;
                        
                        stars += starColor * brightness * 0.4;  // Half the brightness multiplier
                    }
                }
            }
        }
    }
    
    return stars;
}

// Calculate sky gradient
vec3 calculateSkyGradient(vec3 rayDir, vec3 sunDir) {
    float height = rayDir.y;
    float sunHeight = -sunDir.y;
    
    // Day colors - brighter to work with bright stars
    vec3 daySky = vec3(0.5, 0.7, 1.0);
    vec3 dayHorizon = vec3(0.8, 0.9, 1.0);
    
    // Night colors - darker to make stars stand out
    vec3 nightSky = vec3(0.01, 0.01, 0.05);
    vec3 nightHorizon = vec3(0.02, 0.02, 0.08);
    
    // Sunset/sunrise colors
    vec3 sunsetSky = vec3(0.3, 0.2, 0.4);
    vec3 sunsetHorizon = vec3(1.0, 0.5, 0.2);
    
    vec3 skyColor, horizonColor;
    
    if (sunHeight > 0.3) {
        // Daytime
        float t = clamp((sunHeight - 0.3) / 0.7, 0.0, 1.0);
        skyColor = mix(sunsetSky, daySky, t);
        horizonColor = mix(sunsetHorizon, dayHorizon, t);
    } else if (sunHeight > -0.3) {
        // Sunset/sunrise
        skyColor = sunsetSky;
        horizonColor = sunsetHorizon;
    } else {
        // Night
        float t = clamp((-sunHeight - 0.3) / 0.7, 0.0, 1.0);
        skyColor = mix(sunsetSky, nightSky, t);
        horizonColor = mix(sunsetHorizon, nightHorizon, t);
    }
    
    // Vertical gradient
    float gradientT = smoothstep(-0.5, 0.8, height);
    return mix(horizonColor, skyColor, gradientT);
}

// Calculate sun disc and glow
vec3 calculateSunDisc(vec3 rayDir, vec3 sunDir, float sunIntensity) {
    // Angular distance from ray to sun direction (using dot product)
    // This gives us the cosine of the angle between the vectors
    float alignment = dot(rayDir, -sunDir);
    
    // Convert to angular distance (0 = aligned, 1 = perpendicular, 2 = opposite)
    float angularDist = acos(clamp(alignment, -1.0, 1.0));
    
    // Sun disc (sharp falloff)
    float sunDisc = 1.0 - smoothstep(0.0, uSunSize, angularDist);
    
    // Sun glow (wider, softer falloff)
    float sunGlow = 1.0 - smoothstep(0.0, uSunSize * uSunGlow, angularDist);
    sunGlow = pow(sunGlow, 2.0);
    
    // Sun always stays bright and white/yellow
    vec3 sunColor = vec3(1.0, 0.95, 0.8);  // Bright white-yellow
    
    // Make sun completely opaque with strong disc
    vec3 sun = sunColor * (sunDisc * 50.0 + sunGlow * 2.0) * sunIntensity;
    
    return sun;
}

// Calculate moon disc
vec3 calculateMoonDisc(vec3 rayDir, vec3 moonDir, float moonIntensity) {
    // Angular distance from ray to moon direction (using dot product)
    float alignment = dot(rayDir, -moonDir);
    
    // Convert to angular distance
    float angularDist = acos(clamp(alignment, -1.0, 1.0));
    
    // Moon disc (sharp falloff, no glow)
    float moonDisc = 1.0 - smoothstep(0.0, uMoonSize, angularDist);
    
    // Moon color: bluish-white, dimmer than sun
    vec3 moonColor = vec3(0.9, 0.95, 1.0);  // Slight blue tint
    
    // Moon disc brightness - much dimmer than sun
    vec3 moon = moonColor * moonDisc * 8.0 * moonIntensity;
    
    return moon;
}

void main() {
    vec3 rayDir = normalize(vWorldPos);
    
    // Base sky gradient
    vec3 skyColor = calculateSkyGradient(rayDir, uSunDir);
    
    // Add moon disc first (so sun can eclipse it)
    vec3 moonContribution = calculateMoonDisc(rayDir, uMoonDir, uMoonIntensity);
    
    // Add sun disc and glow (renders on top of moon)
    vec3 sunContribution = calculateSunDisc(rayDir, uSunDir, uSunIntensity);
    
    // Add stars (always visible, but blend with sky color like real life)
    vec3 starContribution = generateStars(rayDir, skyColor);
    
    // Combine all elements (moon first, then sun on top for eclipse capability)
    vec3 finalColor = skyColor + moonContribution + sunContribution + starContribution;
    
    // Apply exposure
    finalColor *= uExposure;
    
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
    
    if (m_cubeVAO) {
        glDeleteVertexArrays(1, &m_cubeVAO);
        m_cubeVAO = 0;
    }
    
    if (m_cubeVBO) {
        glDeleteBuffers(1, &m_cubeVBO);
        m_cubeVBO = 0;
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
    m_loc_moonDir = glGetUniformLocation(m_shader, "uMoonDir");
    m_loc_moonIntensity = glGetUniformLocation(m_shader, "uMoonIntensity");
    m_loc_timeOfDay = glGetUniformLocation(m_shader, "uTimeOfDay");
    m_loc_cameraPos = glGetUniformLocation(m_shader, "uCameraPos");
    m_loc_sunSize = glGetUniformLocation(m_shader, "uSunSize");
    m_loc_sunGlow = glGetUniformLocation(m_shader, "uSunGlow");
    m_loc_moonSize = glGetUniformLocation(m_shader, "uMoonSize");
    m_loc_exposure = glGetUniformLocation(m_shader, "uExposure");
    m_loc_projection = glGetUniformLocation(m_shader, "uProjection");
    m_loc_view = glGetUniformLocation(m_shader, "uView");
    
    return true;
}

bool SkyRenderer::createGeometry() {
    // Skybox cube vertices (unit cube centered at origin)
    float cubeVertices[] = {
        // Back face
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        
        // Front face
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        
        // Left face
        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,
        
        // Right face
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
        
        // Bottom face
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,
        
        // Top face
        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f
    };
    
    unsigned int indices[] = {
        // Back face
        0, 1, 2,    2, 3, 0,
        // Front face
        4, 5, 6,    6, 7, 4,
        // Left face
        8, 9, 10,   10, 11, 8,
        // Right face
        12, 13, 14, 14, 15, 12,
        // Bottom face
        16, 17, 18, 18, 19, 16,
        // Top face
        20, 21, 22, 22, 23, 20
    };
    
    // Create VAO
    glGenVertexArrays(1, &m_cubeVAO);
    glGenBuffers(1, &m_cubeVBO);
    GLuint cubeEBO;
    glGenBuffers(1, &cubeEBO);
    
    glBindVertexArray(m_cubeVAO);
    
    // Vertex buffer
    glBindBuffer(GL_ARRAY_BUFFER, m_cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), cubeVertices, GL_STATIC_DRAW);
    
    // Index buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    // Position attribute (3D positions)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glBindVertexArray(0);
    
    return true;
}

void SkyRenderer::render(const glm::vec3& sunDirection, float sunIntensity, 
                        const glm::vec3& moonDirection, float moonIntensity,
                        const glm::vec3& cameraPosition, 
                        const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, float timeOfDay) {
    if (!m_initialized) return;
    
    // Use sky shader
    glUseProgram(m_shader);
    
    // Update uniforms
    updateUniforms(sunDirection, sunIntensity, moonDirection, moonIntensity, 
                  cameraPosition, viewMatrix, projectionMatrix, timeOfDay);
    
    // Set render state for skybox
    glDepthMask(GL_FALSE);        // Don't write to depth buffer
    glDepthFunc(GL_LEQUAL);       // Accept pixels at far plane (depth = 1.0)
    glEnable(GL_DEPTH_TEST);      // Use depth test to only render sky pixels
    glDisable(GL_BLEND);          // Replace the background completely
    glDisable(GL_CULL_FACE);      // Render both sides of cube faces
    
    // Render skybox cube
    glBindVertexArray(m_cubeVAO);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0); // 6 faces * 2 triangles * 3 vertices
    glBindVertexArray(0);
    
    // Restore render state
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);         // Restore normal depth testing
    glEnable(GL_CULL_FACE);       // Restore face culling
    
    glUseProgram(0);
}

void SkyRenderer::updateUniforms(const glm::vec3& sunDirection, float sunIntensity,
                                 const glm::vec3& moonDirection, float moonIntensity,
                                 const glm::vec3& cameraPosition,
                                 const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, float timeOfDay) {
    if (m_loc_sunDir >= 0)
        glUniform3fv(m_loc_sunDir, 1, &sunDirection[0]);
    
    if (m_loc_sunIntensity >= 0)
        glUniform1f(m_loc_sunIntensity, sunIntensity);
    
    if (m_loc_moonDir >= 0)
        glUniform3fv(m_loc_moonDir, 1, &moonDirection[0]);
    
    if (m_loc_moonIntensity >= 0)
        glUniform1f(m_loc_moonIntensity, moonIntensity);
    
    if (m_loc_timeOfDay >= 0)
        glUniform1f(m_loc_timeOfDay, timeOfDay);
    
    if (m_loc_cameraPos >= 0)
        glUniform3fv(m_loc_cameraPos, 1, &cameraPosition[0]);
    
    if (m_loc_sunSize >= 0)
        glUniform1f(m_loc_sunSize, m_sunSize);
    
    if (m_loc_sunGlow >= 0)
        glUniform1f(m_loc_sunGlow, m_sunGlow);
    
    if (m_loc_moonSize >= 0)
        glUniform1f(m_loc_moonSize, m_moonSize);
    
    if (m_loc_exposure >= 0)
        glUniform1f(m_loc_exposure, m_exposure);
    
    if (m_loc_projection >= 0)
        glUniformMatrix4fv(m_loc_projection, 1, GL_FALSE, &projectionMatrix[0][0]);
    
    if (m_loc_view >= 0)
        glUniformMatrix4fv(m_loc_view, 1, GL_FALSE, &viewMatrix[0][0]);
}