// FluidParticleRenderer.h - Simple cube renderer for fluid particles
#pragma once

#include "../Math/Vec3.h"
#include "../ECS/ECS.h"
#include <glad/gl.h>
#include <vector>

// Renders fluid particles as simple semi-transparent cubes
class FluidParticleRenderer {
public:
    FluidParticleRenderer();
    ~FluidParticleRenderer();

    // Initialize GPU resources
    bool initialize();
    
    // Render all fluid particles
    void render(ECSWorld* ecsWorld, const float* viewMatrix, const float* projectionMatrix);
    
    // Cleanup GPU resources
    void shutdown();

private:
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_ebo = 0;
    GLuint m_shader = 0;
    
    bool m_initialized = false;
    
    // Compile simple shader for particle rendering
    bool compileShader();
    
    // Generate cube mesh data
    void setupCubeMesh();
};
