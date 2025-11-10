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
 * Manages post-processing effects that operate on HDR textures
 * from the deferred lighting pass.
 * 
 * Supports:
 * - ACES tone mapping
 * - Gamma correction
 */
class PostProcessingPipeline {
public:
    PostProcessingPipeline();
    ~PostProcessingPipeline();

    bool initialize(int width, int height);
    void shutdown();
    bool resize(int width, int height);

    /**
     * Process the input HDR texture through tone mapping
     * @param inputTexture The HDR input texture from lighting pass (must not be 0)
     * @param depthTexture Depth buffer (unused, kept for API compatibility)
     * @param sunDirection Sun direction (unused, kept for API compatibility)
     * @param cameraPosition Camera position (unused, kept for API compatibility)
     * @param viewProjectionMatrix View-projection matrix (unused, kept for API compatibility)
     */
    void process(GLuint inputTexture, GLuint depthTexture, const glm::vec3& sunDirection, 
                const glm::vec3& cameraPosition, const glm::mat4& viewProjectionMatrix);

    // Effect configuration
    void setExposure(float exposure) { m_exposure = exposure; }
    void setGamma(float gamma) { m_gamma = gamma; }
    void setEnabled(bool enabled) { m_enabled = enabled; }

    bool isEnabled() const { return m_enabled; }
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

    // Render pass
    void renderToneMapping(GLuint inputTexture);

    // OpenGL objects
    GLuint m_finalTexture = 0;          // Final LDR output
    GLuint m_finalFBO = 0;              // Framebuffer for final texture

    // Shaders
    GLuint m_toneMappingShader = 0;     // HDR->LDR tone mapping shader

    // Geometry (fullscreen quad)
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;

    // Uniform locations for tone mapping shader
    GLint m_tone_loc_hdrTexture = -1;
    GLint m_tone_loc_exposure = -1;
    GLint m_tone_loc_gamma = -1;

    // Pipeline configuration
    float m_exposure = EngineParameters::PostProcessing::HDR_EXPOSURE;
    float m_gamma = EngineParameters::PostProcessing::GAMMA_CORRECTION;

    // Framebuffer dimensions
    int m_width = 0;
    int m_height = 0;
    bool m_initialized = false;
    bool m_enabled = true;  // Allow disabling post-processing for debugging
};

// Global post-processing pipeline
extern PostProcessingPipeline g_postProcessing;