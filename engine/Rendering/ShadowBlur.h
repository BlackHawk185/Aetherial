#pragma once

#include <glm/glm.hpp>

using GLuint = unsigned int;
using GLint = int;

/**
 * Shadow Bilateral Blur
 * 
 * Applies a depth-aware bilateral blur to smooth shadow edges while preserving
 * geometry boundaries. This is much cheaper than high-sample-count PCF and gives
 * that smooth Minecraft-style shadow look.
 * 
 * Uses:
 * - Bilateral filtering (blur + edge preservation)
 * - Depth buffer to prevent bleeding across geometry
 * - Two-pass separable blur for performance
 */
class ShadowBlur {
public:
    ShadowBlur();
    ~ShadowBlur();

    bool initialize(int width, int height);
    void shutdown();
    bool resize(int width, int height);

    /**
     * Apply bilateral blur to the lit scene
     * @param inputTexture HDR texture with lighting applied
     * @param depthTexture Depth buffer from G-buffer
     * @return Blurred output texture
     */
    GLuint process(GLuint inputTexture, GLuint depthTexture);

    // Get output FBO for blitting
    GLuint getOutputFBO() const { return m_verticalFBO; }

    // Configuration
    void setBlurRadius(float radius) { m_blurRadius = radius; }
    void setDepthThreshold(float threshold) { m_depthThreshold = threshold; }
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

private:
    bool createShaders();
    bool createFramebuffers();
    void cleanup();

    int m_width = 0;
    int m_height = 0;
    bool m_enabled = true;
    float m_blurRadius = 2.0f;          // Blur radius in pixels
    float m_depthThreshold = 0.01f;     // Depth difference threshold for edge detection

    // Ping-pong framebuffers for two-pass blur
    GLuint m_horizontalFBO = 0;
    GLuint m_horizontalTexture = 0;
    GLuint m_verticalFBO = 0;
    GLuint m_verticalTexture = 0;

    // Shader programs
    GLuint m_horizontalProgram = 0;
    GLuint m_verticalProgram = 0;

    // Uniform locations
    GLint m_h_loc_inputTexture = -1;
    GLint m_h_loc_depthTexture = -1;
    GLint m_h_loc_blurRadius = -1;
    GLint m_h_loc_depthThreshold = -1;
    GLint m_h_loc_texelSize = -1;

    GLint m_v_loc_inputTexture = -1;
    GLint m_v_loc_depthTexture = -1;
    GLint m_v_loc_blurRadius = -1;
    GLint m_v_loc_depthThreshold = -1;
    GLint m_v_loc_texelSize = -1;

    // Fullscreen quad
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;
};

// Global shadow blur instance
extern ShadowBlur g_shadowBlur;
