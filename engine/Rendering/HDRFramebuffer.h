#pragma once

#include <glm/glm.hpp>

using GLuint = unsigned int;
using GLint = int;

/**
 * HDR Framebuffer
 * 
 * Manages an HDR color buffer for intermediate rendering stages.
 * Used between deferred lighting and post-processing to preserve
 * high dynamic range values for proper tone mapping.
 */
class HDRFramebuffer {
public:
    HDRFramebuffer();
    ~HDRFramebuffer();

    bool initialize(int width, int height);
    void shutdown();
    bool resize(int width, int height);

    void bind();
    void unbind();
    void clear();

    GLuint getColorTexture() const { return m_colorTexture; }
    GLuint getDepthTexture() const { return m_depthTexture; }
    GLuint getFBO() const { return m_fbo; }

private:
    void createTextures();
    void deleteTextures();

    GLuint m_fbo = 0;
    GLuint m_colorTexture = 0;  // RGB16F for HDR color
    GLuint m_depthTexture = 0;  // Depth buffer for forward rendering

    int m_width = 0;
    int m_height = 0;
};

// Global HDR framebuffer for lighting output
extern HDRFramebuffer g_hdrFramebuffer;