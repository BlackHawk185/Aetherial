#pragma once

#include "Parameters.h"  // For centralized engine parameters
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <memory>

using GLuint = unsigned int;
using GLint = int;

/**
 * Post-Processing Pipeline
 * 
 * Manages a chain of post-processing effects that operate on HDR textures
 * from the deferred lighting pass. Effects are rendered to intermediate
 * textures and can be chained together.
 * 
 * Supports:
 * - HDR input from deferred lighting
 * - Godrays/volumetric lighting
 * - ACES tone mapping
 * - Gamma correction
 * - Proper sun screen-space projection
 */
class PostProcessingPipeline {
public:
    PostProcessingPipeline();
    ~PostProcessingPipeline();

    bool initialize(int width, int height);
    void shutdown();
    bool resize(int width, int height);

    /**
     * Process the input HDR texture through the post-processing chain
     * @param inputTexture The HDR input texture from lighting pass (must not be 0)
     * @param depthTexture Depth buffer for depth-dependent effects like godrays  
     * @param sunDirection Direction TO the sun (normalized)
     * @param cameraPosition Current camera position
     * @param viewProjectionMatrix Combined view-projection matrix for accurate sun screen position
     */
    void process(GLuint inputTexture, GLuint depthTexture, const glm::vec3& sunDirection, 
                const glm::vec3& cameraPosition, const glm::mat4& viewProjectionMatrix);

    // Effect configuration
    void setExposure(float exposure) { m_exposure = exposure; }
    void setGamma(float gamma) { m_gamma = gamma; }
    void setGodrayIntensity(float intensity) { m_godrayIntensity = intensity; }
    void setGodrayDecay(float decay) { m_godrayDecay = decay; }
    void setGodrayDensity(float density) { m_godrayDensity = density; }
    void setGodrayWeight(float weight) { m_godrayWeight = weight; }
    void setEnabled(bool enabled) { m_enabled = enabled; }

    bool isEnabled() const { return m_enabled; }
    float getGodrayIntensity() const { return m_godrayIntensity; }
    float getExposure() const { return m_exposure; }
    float getGamma() const { return m_gamma; }

    // Get final output texture
    GLuint getFinalTexture() const { return m_finalTexture; }

private:
    bool createFramebuffers();
    bool createShaders();
    bool createGeometry();
    void deleteFramebuffers();
    void deleteShaders();

    // Render passes
    void renderGodrays(GLuint inputTexture, GLuint depthTexture, const glm::vec3& sunDirection, 
                      const glm::vec3& cameraPosition, const glm::mat4& viewProjectionMatrix);
    void renderToneMapping(GLuint inputTexture);

    // OpenGL objects
    GLuint m_intermediateTexture = 0;   // For intermediate effects
    GLuint m_finalTexture = 0;          // Final LDR output
    GLuint m_intermediateFBO = 0;       // Framebuffer for intermediate texture
    GLuint m_finalFBO = 0;              // Framebuffer for final texture

    // Shaders
    GLuint m_godrayShader = 0;          // Volumetric lighting shader
    GLuint m_toneMappingShader = 0;     // HDR->LDR tone mapping shader

    // Geometry (fullscreen quad)
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;

    // Uniform locations for godray shader
    GLint m_godray_loc_inputTexture = -1;
    GLint m_godray_loc_depthTexture = -1;
    GLint m_godray_loc_sunDirection = -1;
    GLint m_godray_loc_cameraPosition = -1;
    GLint m_godray_loc_intensity = -1;
    GLint m_godray_loc_decay = -1;
    GLint m_godray_loc_density = -1;
    GLint m_godray_loc_weight = -1;
    GLint m_godray_loc_viewProjectionMatrix = -1;

    // Uniform locations for tone mapping shader
    GLint m_tone_loc_hdrTexture = -1;
    GLint m_tone_loc_exposure = -1;
    GLint m_tone_loc_gamma = -1;

    // Pipeline configuration - now using centralized parameters
    float m_exposure = EngineParameters::PostProcessing::HDR_EXPOSURE;
    float m_gamma = EngineParameters::PostProcessing::GAMMA_CORRECTION;
    float m_godrayIntensity = EngineParameters::PostProcessing::GODRAY_INTENSITY;
    float m_godrayDecay = EngineParameters::PostProcessing::GODRAY_DECAY;
    float m_godrayDensity = EngineParameters::PostProcessing::GODRAY_DENSITY;
    float m_godrayWeight = EngineParameters::PostProcessing::GODRAY_WEIGHT;

    // Framebuffer dimensions
    int m_width = 0;
    int m_height = 0;
    bool m_initialized = false;
    bool m_enabled = true;  // Allow disabling post-processing for debugging
};

// Global post-processing pipeline
extern PostProcessingPipeline g_postProcessing;