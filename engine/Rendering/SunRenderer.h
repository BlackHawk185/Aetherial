#pragma once

#include <glm/glm.hpp>

using GLuint = unsigned int;
using GLint = int;

/**
 * Sun Renderer
 * 
 * Simple post-processing effect that draws a sun disc in the sky.
 * Much simpler than trying to integrate into deferred lighting.
 * Renders after the main scene but before UI elements.
 */
class SunRenderer {
public:
    SunRenderer();
    ~SunRenderer();

    bool initialize();
    void shutdown();
    
    /**
     * Render the sun disc as an overlay
     * @param sunDirection Direction TO the sun (normalized)
     * @param cameraView Current camera view matrix
     * @param cameraProjection Current camera projection matrix
     * @param screenWidth Screen width in pixels
     * @param screenHeight Screen height in pixels
     */
    void render(const glm::vec3& sunDirection, 
                const glm::mat4& cameraView, 
                const glm::mat4& cameraProjection,
                int screenWidth, 
                int screenHeight);

    // Configuration
    void setSunSize(float size) { m_sunSize = size; }
    void setSunIntensity(float intensity) { m_sunIntensity = intensity; }

private:
    bool createShader();
    bool createGeometry();
    void deleteShader();

    // OpenGL objects
    GLuint m_shader = 0;
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;

    // Shader uniform locations
    GLint m_loc_sunScreenPos = -1;
    GLint m_loc_sunSize = -1;
    GLint m_loc_sunColor = -1;
    GLint m_loc_sunIntensity = -1;
    GLint m_loc_screenSize = -1;

    // Configuration
    float m_sunSize = 0.02f;        // Size of sun disc (0.01 = small, 0.05 = large)
    float m_sunIntensity = 1.0f;    // Brightness multiplier

    bool m_initialized = false;
};

// Global sun renderer
extern SunRenderer g_sunRenderer;