#pragma once

#include <glm/glm.hpp>

using GLuint = unsigned int;
using GLint = int;

/**
 * Sky Renderer
 * 
 * Renders atmospheric effects:
 * - Sky gradient (day/night cycle)
 * - Visible sun disc
 * - Sun glow/halo effects
 * - Optional moon and stars (future)
 * 
 * This is a forward renderer that draws over the background
 * after G-buffer lighting but before post-processing.
 */
class SkyRenderer {
public:
    SkyRenderer();
    ~SkyRenderer();

    bool initialize();
    void shutdown();

    /**
     * Render sky with sun disc
     * @param sunDirection Direction TO the sun (normalized)
     * @param sunIntensity 0.0 (night) to 1.0 (day)
     * @param cameraPosition Current camera position for any distance effects
     * @param viewMatrix Camera view matrix for proper ray direction calculation
     * @param aspectRatio Screen aspect ratio for proper projection
     */
    void render(const glm::vec3& sunDirection, float sunIntensity, const glm::vec3& cameraPosition,
               const glm::mat4& viewMatrix, float aspectRatio);

    // Sky appearance parameters
    void setSunSize(float size) { m_sunSize = size; }
    void setSunGlow(float glow) { m_sunGlow = glow; }
    void setExposure(float exposure) { m_exposure = exposure; }

private:
    bool createShaders();
    bool createGeometry();
    void updateUniforms(const glm::vec3& sunDirection, float sunIntensity, const glm::vec3& cameraPosition,
                       const glm::mat4& viewMatrix, float aspectRatio);

    // OpenGL objects
    GLuint m_shader = 0;
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;

    // Uniform locations
    GLint m_loc_sunDir = -1;
    GLint m_loc_sunIntensity = -1;
    GLint m_loc_cameraPos = -1;
    GLint m_loc_sunSize = -1;
    GLint m_loc_sunGlow = -1;
    GLint m_loc_exposure = -1;
    GLint m_loc_aspectRatio = -1;
    GLint m_loc_viewMatrix = -1;

    // Sky parameters
    float m_sunSize = 0.1f;      // Angular size of sun disc (doubled from 0.02f)
    float m_sunGlow = 4.0f;       // Glow radius multiplier
    float m_exposure = 1.0f;      // Sky brightness multiplier

    bool m_initialized = false;
};

// Global sky renderer
extern SkyRenderer g_skyRenderer;