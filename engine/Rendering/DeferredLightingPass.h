#pragma once

#include <glm/glm.hpp>

using GLuint = unsigned int;
using GLint = int;

/**
 * Deferred Lighting Pass
 * 
 * Reads G-buffer textures and applies:
 * - Cascaded shadow mapping (CSM)
 * - Directional sun lighting
 * - Day/night cycle
 * 
 * Outputs final lit color to screen
 */
class DeferredLightingPass {
public:
    DeferredLightingPass();
    ~DeferredLightingPass();

    bool initialize();
    void shutdown();

    // Render full-screen quad with deferred lighting to HDR framebuffer
    void render(const glm::vec3& sunDirection, const glm::vec3& cameraPosition);

    // Update cascade shadow map data
    void setCascadeData(int index, const glm::mat4& viewProj, float splitDistance);

private:
    GLuint m_shader = 0;
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;

    // Shader uniform locations (cached for performance)
    GLint m_loc_gAlbedo = -1;
    GLint m_loc_gNormal = -1;
    GLint m_loc_gPosition = -1;
    GLint m_loc_gMetadata = -1;
    GLint m_loc_gDepth = -1;
    GLint m_loc_shadowMap = -1;
    GLint m_loc_sunDir = -1;
    GLint m_loc_cameraPos = -1;
    GLint m_loc_cascadeVP = -1;
    GLint m_loc_numCascades = -1;
    GLint m_loc_shadowTexel = -1;

    // Cascade data
    glm::mat4 m_cascadeVP[2];
    float m_cascadeSplits[2];
};

// Global deferred lighting pass
extern DeferredLightingPass g_deferredLighting;
