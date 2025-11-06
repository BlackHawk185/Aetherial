#pragma once

#include <glm/glm.hpp>

using GLuint = unsigned int;

/**
 * G-Buffer for deferred rendering
 * 
 * Layout (MRT - Multiple Render Targets):
 * - Texture 0 (RGB16F): Albedo/Base Color
 * - Texture 1 (RGB16F): World-Space Normal
 * - Texture 2 (RGB32F): World Position (high precision for large worlds)
 * - Texture 3 (RGBA8):  BlockType (R), FaceDir (G), unused (B/A)
 * - Depth (D24S8):      Scene depth buffer
 */
class GBuffer {
public:
    GBuffer();
    ~GBuffer();

    bool initialize(int width, int height);
    void shutdown();
    bool resize(int width, int height);

    // Bind for geometry pass (write to G-buffer)
    void bindForGeometryPass();
    
    // Bind for lighting pass (read from G-buffer)
    void bindForLightingPass();
    
    // Unbind (restore default framebuffer)
    void unbind();

    // Texture getters for binding in shaders
    GLuint getAlbedoTexture() const { return m_albedoTex; }
    GLuint getNormalTexture() const { return m_normalTex; }
    GLuint getPositionTexture() const { return m_positionTex; }
    GLuint getMetadataTexture() const { return m_metadataTex; }
    GLuint getDepthTexture() const { return m_depthTex; }

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

private:
    void createTextures();
    void deleteTextures();

    int m_width = 0;
    int m_height = 0;

    GLuint m_fbo = 0;
    GLuint m_albedoTex = 0;    // RGB16F - base color
    GLuint m_normalTex = 0;    // RGB16F - world-space normal
    GLuint m_positionTex = 0;  // RGB32F - world position
    GLuint m_metadataTex = 0;  // RGBA8 - block metadata
    GLuint m_depthTex = 0;     // D24S8 - depth/stencil
};

// Global G-buffer instance
extern GBuffer g_gBuffer;
