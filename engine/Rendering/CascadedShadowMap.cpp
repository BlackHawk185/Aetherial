#include "CascadedShadowMap.h"
#include <glad/gl.h>
#include <iostream>

ShadowMap g_shadowMap;

bool ShadowMap::initialize(int size, int numCascades)
{
    shutdown();
    m_size = size;
    m_numCascades = numCascades;
    m_cascades.resize(m_numCascades);

    // Create 2D array texture for all cascades
    glGenTextures(1, &m_depthTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_depthTex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, m_size, m_size, m_numCascades, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    // We'll attach individual layers when rendering each cascade
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    
    std::cout << "✅ CSM initialized: " << m_numCascades << " cascades @ " 
              << m_size << "x" << m_size << std::endl;
    
    return true;
}

void ShadowMap::shutdown()
{
    if (m_fbo) { glDeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
    if (m_depthTex) { glDeleteTextures(1, &m_depthTex); m_depthTex = 0; }
    m_size = 0;
    m_cascades.clear();
}

void ShadowMap::begin(int cascadeIndex)
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    
    // Attach specific layer of the array texture
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_depthTex, 0, cascadeIndex);
    
    glViewport(0, 0, m_size, m_size);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    
    // Polygon offset disabled - using slope-scale bias in shader instead for better quality
    // glEnable(GL_POLYGON_OFFSET_FILL);
    // glPolygonOffset(2.0f, 4.0f);
    
    // Disable face culling for shadow pass - render all geometry from light's POV
    // Ensures shadows are cast correctly regardless of triangle orientation
    glDisable(GL_CULL_FACE);
}

GLuint ShadowMap::getDepthTexture(int cascadeIndex) const
{
    // For array textures, we return the same texture ID
    // The shader will sample using the cascade index as the layer
    return m_depthTex;
}

bool ShadowMap::resize(int newSize)
{
    // Delete existing
    if (m_fbo) { glDeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
    if (m_depthTex) { glDeleteTextures(1, &m_depthTex); m_depthTex = 0; }

    m_size = newSize;
    
    // Recreate array texture
    glGenTextures(1, &m_depthTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_depthTex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, m_size, m_size, m_numCascades, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return true;
}

void ShadowMap::end(int screenWidth, int screenHeight)
{
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // DON'T re-enable culling here - we might be rendering multiple cascades in sequence
    // Caller will restore culling state after ALL cascades are rendered
    
    // Restore default framebuffer draw/read buffers to avoid accidental color suppression
    glDrawBuffer(GL_BACK);
    glReadBuffer(GL_BACK);
    glViewport(0, 0, screenWidth, screenHeight);
}
