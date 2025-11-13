#pragma once

#include <glm/glm.hpp>
#include <vector>
using GLuint = unsigned int;

struct CascadeData {
    glm::mat4 viewProj;
    float splitDistance;
    float orthoSize;
};

class LightMap {
public:
    bool initialize(int size = 16384, int numCascades = 2);
    void shutdown();

    int getSize() const { return m_size; }
    int getNumCascades() const { return m_numCascades; }
    bool resize(int newSize);

    void bindForRendering(int cascadeIndex);
    void unbindAfterRendering(int screenWidth, int screenHeight);

    GLuint getDepthTexture() const { return m_depthTex; }
    
    const CascadeData& getCascade(int index) const { return m_cascades[index]; }
    void setCascadeData(int index, const CascadeData& data) { m_cascades[index] = data; }

private:
    int m_size = 0;
    int m_numCascades = 2;
    GLuint m_fbo = 0;
    GLuint m_depthTex = 0;
    std::vector<CascadeData> m_cascades;
};

extern LightMap g_lightMap;
