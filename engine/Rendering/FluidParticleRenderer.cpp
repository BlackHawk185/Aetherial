// FluidParticleRenderer.cpp - Simple cube renderer for fluid particles
#include "FluidParticleRenderer.h"
#include "../World/FluidSystem.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

FluidParticleRenderer::FluidParticleRenderer() {
}

FluidParticleRenderer::~FluidParticleRenderer() {
    shutdown();
}

bool FluidParticleRenderer::initialize() {
    if (m_initialized) return true;
    
    if (!compileShader()) {
        std::cerr << "Failed to compile fluid particle shader" << std::endl;
        return false;
    }
    
    setupCubeMesh();
    
    m_initialized = true;
    std::cout << "FluidParticleRenderer initialized" << std::endl;
    return true;
}

void FluidParticleRenderer::setupCubeMesh() {
    // Simple cube vertices (0.4 block size to match particle radius)
    const float size = 0.4f;
    float vertices[] = {
        // Positions (x, y, z)
        -size, -size, -size,
         size, -size, -size,
         size,  size, -size,
        -size,  size, -size,
        -size, -size,  size,
         size, -size,  size,
         size,  size,  size,
        -size,  size,  size,
    };
    
    // Cube indices (triangles)
    unsigned int indices[] = {
        // Back face
        0, 1, 2,  2, 3, 0,
        // Front face
        4, 5, 6,  6, 7, 4,
        // Left face
        0, 3, 7,  7, 4, 0,
        // Right face
        1, 5, 6,  6, 2, 1,
        // Bottom face
        0, 1, 5,  5, 4, 0,
        // Top face
        3, 2, 6,  6, 7, 3
    };
    
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);
    
    glBindVertexArray(m_vao);
    
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glBindVertexArray(0);
}

bool FluidParticleRenderer::compileShader() {
    const char* vertexShaderSource = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;
        
        uniform mat4 model;
        uniform mat4 view;
        uniform mat4 projection;
        
        void main() {
            gl_Position = projection * view * model * vec4(aPos, 1.0);
        }
    )";
    
    const char* fragmentShaderSource = R"(
        #version 330 core
        out vec4 FragColor;
        
        uniform vec4 particleColor;
        
        void main() {
            FragColor = particleColor;
        }
    )";
    
    // Compile vertex shader
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr);
    glCompileShader(vertexShader);
    
    GLint success;
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        std::cerr << "Vertex shader compilation failed:\n" << infoLog << std::endl;
        return false;
    }
    
    // Compile fragment shader
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr);
    glCompileShader(fragmentShader);
    
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        std::cerr << "Fragment shader compilation failed:\n" << infoLog << std::endl;
        return false;
    }
    
    // Link shader program
    m_shader = glCreateProgram();
    glAttachShader(m_shader, vertexShader);
    glAttachShader(m_shader, fragmentShader);
    glLinkProgram(m_shader);
    
    glGetProgramiv(m_shader, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_shader, 512, nullptr, infoLog);
        std::cerr << "Shader program linking failed:\n" << infoLog << std::endl;
        return false;
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    return true;
}

void FluidParticleRenderer::render(ECSWorld* ecsWorld, const float* viewMatrix, const float* projectionMatrix) {
    if (!m_initialized || !ecsWorld) return;
    
    // Get all fluid particles
    auto* fluidStorage = ecsWorld->getStorage<FluidParticleComponent>();
    auto* transformStorage = ecsWorld->getStorage<TransformComponent>();
    
    if (!fluidStorage || !transformStorage) return;
    if (fluidStorage->entities.empty()) return;
    
    // Enable blending for semi-transparent particles
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUseProgram(m_shader);
    glBindVertexArray(m_vao);
    
    // Set view and projection matrices
    GLint viewLoc = glGetUniformLocation(m_shader, "view");
    GLint projLoc = glGetUniformLocation(m_shader, "projection");
    GLint modelLoc = glGetUniformLocation(m_shader, "model");
    GLint colorLoc = glGetUniformLocation(m_shader, "particleColor");
    
    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, viewMatrix);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, projectionMatrix);
    
    // Semi-transparent blue color for water particles
    glUniform4f(colorLoc, 0.2f, 0.5f, 0.9f, 0.6f);
    
    // Render each particle
    for (size_t i = 0; i < fluidStorage->entities.size(); ++i) {
        EntityID entity = fluidStorage->entities[i];
        
        // Get transform
        TransformComponent* transform = transformStorage->getComponent(entity);
        if (!transform) continue;
        
        // Create model matrix for this particle
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, glm::vec3(transform->position.x, transform->position.y, transform->position.z));
        
        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
        
        // Draw cube
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
    }
    
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void FluidParticleRenderer::shutdown() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_shader) glDeleteProgram(m_shader);
    
    m_vao = m_vbo = m_ebo = m_shader = 0;
    m_initialized = false;
}
