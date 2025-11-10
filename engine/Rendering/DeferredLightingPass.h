#pragma once

#include <glm/glm.hpp>

using GLuint = unsigned int;
using GLint = int;

/**
 * Deferred Lighting Pass
 * 
 * Reads G-buffer textures and applies:
 * - Cascaded light mapping (4 cascades: 2 sun + 2 moon)
 * - Directional sun and moon lighting
 * - Day/night cycle
 * 
 * Dark by default - only lit where light maps indicate
 * 
 * Outputs final lit color to HDR framebuffer
 */
class DeferredLightingPass {
public:
    DeferredLightingPass();
    ~DeferredLightingPass();

    bool initialize();
    void shutdown();

    // Render full-screen quad with deferred lighting to HDR framebuffer
    void render(const glm::vec3& sunDirection, const glm::vec3& moonDirection, 
                float sunIntensity, float moonIntensity, const glm::vec3& cameraPosition,
                float timeOfDay = 0.0f);

    // Update cascade light map data
    void setCascadeData(int index, const glm::mat4& viewProj, float splitDistance, float orthoSize);

    // Configure shadow dithering strength (0.0 = world-space only, 1.0 = full dithering)
    void setDitherStrength(float strength) { m_ditherStrength = glm::clamp(strength, 0.0f, 1.0f); }
    float getDitherStrength() const { return m_ditherStrength; }
    
    // Enable/disable cloud shadows
    void setCloudShadowsEnabled(bool enabled) { m_cloudShadowsEnabled = enabled; }
    bool areCloudShadowsEnabled() const { return m_cloudShadowsEnabled; }

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
    GLint m_loc_lightMap = -1;      // Renamed from shadowMap (dark by default, lit areas bright)
    GLint m_loc_sunDir = -1;
    GLint m_loc_moonDir = -1;
    GLint m_loc_sunIntensity = -1;
    GLint m_loc_moonIntensity = -1;
    GLint m_loc_cameraPos = -1;
    GLint m_loc_cascadeVP = -1;
    GLint m_loc_numCascades = -1;
    GLint m_loc_lightTexel = -1;    // Renamed from shadowTexel
    GLint m_loc_cascadeOrthoSizes = -1;
    GLint m_loc_ditherStrength = -1;
    GLint m_loc_cloudNoiseTex = -1;
    GLint m_loc_timeOfDay = -1;
    GLint m_loc_enableCloudShadows = -1;

    // Cascade data (4 cascades: sun near, sun far, moon near, moon far)
    glm::mat4 m_cascadeVP[4];
    float m_cascadeSplits[4];
    float m_cascadeOrthoSizes[4];
    
    // Shadow settings
    float m_ditherStrength = 1.0f;  // Default: 75% dithering (balanced)
    bool m_cloudShadowsEnabled = true;
};

// Global deferred lighting pass
extern DeferredLightingPass g_deferredLighting;
