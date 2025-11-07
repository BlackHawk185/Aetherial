#pragma once

#include <glm/glm.hpp>

using GLuint = unsigned int;
using GLint = int;

/**
 * Sky Renderer
 * 
 * Renders a complete skybox cube with:
 * - Dynamic sky gradients (day/night/sunset transitions)
 * - Animated starfield during night
 * - Realistic sun disc with glow effects
 * - Proper depth handling to render behind all geometry
 * 
 * Uses a unit cube with view matrix transformation for
 * optimal performance and proper directional sampling.
 */
class SkyRenderer {
public:
    SkyRenderer();
    ~SkyRenderer();

    bool initialize();
    void shutdown();

    /**
     * Render skybox cube with sun, moon, stars, and gradients
     * @param sunDirection Direction TO the sun (normalized)
     * @param sunIntensity 0.0 (night) to 1.0 (day)
     * @param moonDirection Direction TO the moon (normalized)
     * @param moonIntensity 0.0 (day) to 1.0 (night)
     * @param cameraPosition Current camera position
     * @param viewMatrix Camera view matrix
     * @param projectionMatrix Camera projection matrix
     * @param timeOfDay Time value for star twinkling animation
     */
    void render(const glm::vec3& sunDirection, float sunIntensity, 
               const glm::vec3& moonDirection, float moonIntensity,
               const glm::vec3& cameraPosition,
               const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, float timeOfDay);

    // Sky appearance parameters
    void setSunSize(float size) { m_sunSize = size; }
    void setSunGlow(float glow) { m_sunGlow = glow; }
    void setMoonSize(float size) { m_moonSize = size; }
    void setExposure(float exposure) { m_exposure = exposure; }

private:
    bool createShaders();
    bool createGeometry();
    void updateUniforms(const glm::vec3& sunDirection, float sunIntensity,
                       const glm::vec3& moonDirection, float moonIntensity,
                       const glm::vec3& cameraPosition,
                       const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, float timeOfDay);

    // OpenGL objects
    GLuint m_shader = 0;
    GLuint m_cubeVAO = 0;
    GLuint m_cubeVBO = 0;

    // Uniform locations
    GLint m_loc_sunDir = -1;
    GLint m_loc_sunIntensity = -1;
    GLint m_loc_moonDir = -1;
    GLint m_loc_moonIntensity = -1;
    GLint m_loc_timeOfDay = -1;
    GLint m_loc_cameraPos = -1;
    GLint m_loc_sunSize = -1;
    GLint m_loc_sunGlow = -1;
    GLint m_loc_moonSize = -1;
    GLint m_loc_exposure = -1;
    GLint m_loc_projection = -1;
    GLint m_loc_view = -1;

    // Sky parameters
    float m_sunSize = 0.1f;       // Angular size of sun disc (doubled from 0.02f)
    float m_sunGlow = 4.0f;       // Glow radius multiplier
    float m_moonSize = 0.08f;     // Angular size of moon disc (slightly smaller than sun)
    float m_exposure = 1.0f;      // Sky brightness multiplier

    bool m_initialized = false;
};

// Global sky renderer
extern SkyRenderer g_skyRenderer;