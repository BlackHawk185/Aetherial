#pragma once

#include <glm/glm.hpp>
#include "Parameters.h"

using GLuint = unsigned int;
using GLint = int;

/**
 * Volumetric Cloud Renderer
 * 
 * Renders realistic volumetric clouds using raymarching through a 3D noise texture.
 * Clouds exist in a defined altitude layer and respond to sun lighting for proper
 * atmospheric integration.
 * 
 * Features:
 * - 3D Perlin/Worley noise for cloud density
 * - Altitude-based cloud layer
 * - Beer-Lambert light absorption
 * - Sun lighting integration
 * - Configurable density, coverage, and detail
 */
class VolumetricCloudRenderer {
public:
    VolumetricCloudRenderer();
    ~VolumetricCloudRenderer();

    bool initialize();
    void shutdown();

    /**
     * Render volumetric clouds
     * @param sunDirection Direction TO the sun (normalized)
     * @param sunIntensity Sun brightness (0.0 to 1.0)
     * @param cameraPosition Current camera position
     * @param viewMatrix Camera view matrix
     * @param projectionMatrix Camera projection matrix
     * @param depthTexture Scene depth buffer for depth testing
     * @param timeOfDay Time value for cloud animation
     */
    void render(const glm::vec3& sunDirection, float sunIntensity,
                const glm::vec3& cameraPosition,
                const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                GLuint depthTexture, float timeOfDay);

    /**
     * Sample cloud density at a world position (for shadow map integration)
     * @param worldPosition Position in world space to sample
     * @param timeOfDay Time value for cloud animation
     * @return Cloud density at position (0.0 = no cloud, 1.0 = full density)
     */
    float sampleCloudDensityAt(const glm::vec3& worldPosition, float timeOfDay);

    // Cloud appearance parameters
    void setCloudCoverage(float coverage) { m_cloudCoverage = coverage; }
    void setCloudDensity(float density) { m_cloudDensity = density; }
    void setCloudSpeed(float speed) { m_cloudSpeed = speed; }
    
    // Get 3D noise texture for external use (shadow map rendering)
    GLuint getNoiseTexture() const { return m_noiseTexture3D; }

private:
    bool createShaders();
    bool createGeometry();
    bool create3DNoiseTexture();
    
    void updateUniforms(const glm::vec3& sunDirection, float sunIntensity,
                       const glm::vec3& cameraPosition,
                       const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                       float timeOfDay);

    // CPU-side Perlin noise for shadow map sampling
    float perlinNoise3D(float x, float y, float z);

    // OpenGL objects
    GLuint m_shader = 0;
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;
    GLuint m_noiseTexture3D = 0;

    // Uniform locations
    GLint m_uViewMatrix = -1;
    GLint m_uProjectionMatrix = -1;
    GLint m_uInvProjectionMatrix = -1;
    GLint m_uInvViewMatrix = -1;
    GLint m_uCameraPosition = -1;
    GLint m_uSunDirection = -1;
    GLint m_uSunIntensity = -1;
    GLint m_uTimeOfDay = -1;
    GLint m_uCloudCoverage = -1;
    GLint m_uCloudDensity = -1;
    GLint m_uCloudSpeed = -1;
    GLint m_uDepthTexture = -1;
    GLint m_uNoiseTexture = -1;

    // Cloud parameters - default from EngineParameters
    float m_cloudCoverage = EngineParameters::Clouds::CLOUD_COVERAGE;
    float m_cloudDensity = EngineParameters::Clouds::CLOUD_DENSITY;
    float m_cloudSpeed = EngineParameters::Clouds::CLOUD_SPEED;
};

// Global cloud renderer instance
extern VolumetricCloudRenderer g_cloudRenderer;
