#include "ShadowBlur.h"
#include <glad/gl.h>
#include <iostream>

ShadowBlur g_shadowBlur;

namespace {
    // Fullscreen quad vertex shader
    static const char* kVS = R"GLSL(
#version 460 core
layout(location = 0) in vec2 aPos;

out vec2 vUV;

void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

    // Horizontal bilateral blur fragment shader
    static const char* kFS_Horizontal = R"GLSL(
#version 460 core
in vec2 vUV;

uniform sampler2D uInputTexture;
uniform sampler2D uDepthTexture;
uniform float uBlurRadius;
uniform float uDepthThreshold;
uniform vec2 uTexelSize;

out vec4 FragColor;

void main() {
    vec3 centerColor = texture(uInputTexture, vUV).rgb;
    float centerDepth = texture(uDepthTexture, vUV).r;
    
    vec3 blurred = centerColor;
    float totalWeight = 1.0;
    
    // Horizontal blur with depth-aware weighting
    int samples = int(uBlurRadius);
    for (int x = -samples; x <= samples; ++x) {
        if (x == 0) continue;
        
        vec2 offset = vec2(float(x) * uTexelSize.x, 0.0);
        vec2 sampleUV = vUV + offset;
        
        // Sample depth and color
        float sampleDepth = texture(uDepthTexture, sampleUV).r;
        vec3 sampleColor = texture(uInputTexture, sampleUV).rgb;
        
        // Bilateral weight based on depth similarity
        float depthDiff = abs(centerDepth - sampleDepth);
        float depthWeight = exp(-depthDiff / uDepthThreshold);
        
        // Spatial Gaussian weight
        float spatialWeight = exp(-float(x * x) / (2.0 * uBlurRadius * uBlurRadius));
        
        // Combined weight
        float weight = depthWeight * spatialWeight;
        
        blurred += sampleColor * weight;
        totalWeight += weight;
    }
    
    FragColor = vec4(blurred / totalWeight, 1.0);
}
)GLSL";

    // Vertical bilateral blur fragment shader
    static const char* kFS_Vertical = R"GLSL(
#version 460 core
in vec2 vUV;

uniform sampler2D uInputTexture;
uniform sampler2D uDepthTexture;
uniform float uBlurRadius;
uniform float uDepthThreshold;
uniform vec2 uTexelSize;

out vec4 FragColor;

void main() {
    vec3 centerColor = texture(uInputTexture, vUV).rgb;
    float centerDepth = texture(uDepthTexture, vUV).r;
    
    vec3 blurred = centerColor;
    float totalWeight = 1.0;
    
    // Vertical blur with depth-aware weighting
    int samples = int(uBlurRadius);
    for (int y = -samples; y <= samples; ++y) {
        if (y == 0) continue;
        
        vec2 offset = vec2(0.0, float(y) * uTexelSize.y);
        vec2 sampleUV = vUV + offset;
        
        // Sample depth and color
        float sampleDepth = texture(uDepthTexture, sampleUV).r;
        vec3 sampleColor = texture(uInputTexture, sampleUV).rgb;
        
        // Bilateral weight based on depth similarity
        float depthDiff = abs(centerDepth - sampleDepth);
        float depthWeight = exp(-depthDiff / uDepthThreshold);
        
        // Spatial Gaussian weight
        float spatialWeight = exp(-float(y * y) / (2.0 * uBlurRadius * uBlurRadius));
        
        // Combined weight
        float weight = depthWeight * spatialWeight;
        
        blurred += sampleColor * weight;
        totalWeight += weight;
    }
    
    FragColor = vec4(blurred / totalWeight, 1.0);
}
)GLSL";

    GLuint CompileShader(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        
        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[512];
            glGetShaderInfoLog(shader, 512, nullptr, log);
            std::cerr << "[ShadowBlur] Shader compilation failed:\n" << log << std::endl;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint LinkProgram(GLuint vs, GLuint fs) {
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        
        GLint success;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            char log[512];
            glGetProgramInfoLog(program, 512, nullptr, log);
            std::cerr << "[ShadowBlur] Program linking failed:\n" << log << std::endl;
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }
}

ShadowBlur::ShadowBlur() = default;

ShadowBlur::~ShadowBlur() {
    shutdown();
}

bool ShadowBlur::initialize(int width, int height) {
    m_width = width;
    m_height = height;

    if (!createShaders()) {
        return false;
    }

    if (!createFramebuffers()) {
        return false;
    }

    // Create fullscreen quad
    const float quadVertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };

    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &m_quadVBO);

    glBindVertexArray(m_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glBindVertexArray(0);

    std::cout << "✅ Shadow bilateral blur initialized: " << width << "x" << height << std::endl;
    return true;
}

void ShadowBlur::shutdown() {
    cleanup();
    
    if (m_horizontalProgram) { glDeleteProgram(m_horizontalProgram); m_horizontalProgram = 0; }
    if (m_verticalProgram) { glDeleteProgram(m_verticalProgram); m_verticalProgram = 0; }
    if (m_quadVAO) { glDeleteVertexArrays(1, &m_quadVAO); m_quadVAO = 0; }
    if (m_quadVBO) { glDeleteBuffers(1, &m_quadVBO); m_quadVBO = 0; }
}

bool ShadowBlur::resize(int width, int height) {
    if (width == m_width && height == m_height) {
        return true;
    }

    m_width = width;
    m_height = height;

    cleanup();
    return createFramebuffers();
}

GLuint ShadowBlur::process(GLuint inputTexture, GLuint depthTexture) {
    if (!m_enabled || inputTexture == 0) {
        return inputTexture;
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glm::vec2 texelSize(1.0f / m_width, 1.0f / m_height);

    // ===== Horizontal pass =====
    glBindFramebuffer(GL_FRAMEBUFFER, m_horizontalFBO);
    glViewport(0, 0, m_width, m_height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_horizontalProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTexture);
    if (m_h_loc_inputTexture >= 0) glUniform1i(m_h_loc_inputTexture, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depthTexture);
    if (m_h_loc_depthTexture >= 0) glUniform1i(m_h_loc_depthTexture, 1);

    if (m_h_loc_blurRadius >= 0) glUniform1f(m_h_loc_blurRadius, m_blurRadius);
    if (m_h_loc_depthThreshold >= 0) glUniform1f(m_h_loc_depthThreshold, m_depthThreshold);
    if (m_h_loc_texelSize >= 0) glUniform2fv(m_h_loc_texelSize, 1, &texelSize[0]);

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // ===== Vertical pass =====
    glBindFramebuffer(GL_FRAMEBUFFER, m_verticalFBO);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_verticalProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_horizontalTexture);
    if (m_v_loc_inputTexture >= 0) glUniform1i(m_v_loc_inputTexture, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depthTexture);
    if (m_v_loc_depthTexture >= 0) glUniform1i(m_v_loc_depthTexture, 1);

    if (m_v_loc_blurRadius >= 0) glUniform1f(m_v_loc_blurRadius, m_blurRadius);
    if (m_v_loc_depthThreshold >= 0) glUniform1f(m_v_loc_depthThreshold, m_depthThreshold);
    if (m_v_loc_texelSize >= 0) glUniform2fv(m_v_loc_texelSize, 1, &texelSize[0]);

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Cleanup
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);

    glEnable(GL_DEPTH_TEST);

    return m_verticalTexture;
}

bool ShadowBlur::createShaders() {
    // Compile horizontal blur shader
    GLuint vs_h = CompileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs_h = CompileShader(GL_FRAGMENT_SHADER, kFS_Horizontal);
    
    if (!vs_h || !fs_h) {
        if (vs_h) glDeleteShader(vs_h);
        if (fs_h) glDeleteShader(fs_h);
        return false;
    }

    m_horizontalProgram = LinkProgram(vs_h, fs_h);
    glDeleteShader(vs_h);
    glDeleteShader(fs_h);

    if (!m_horizontalProgram) {
        return false;
    }

    // Get uniform locations for horizontal pass
    m_h_loc_inputTexture = glGetUniformLocation(m_horizontalProgram, "uInputTexture");
    m_h_loc_depthTexture = glGetUniformLocation(m_horizontalProgram, "uDepthTexture");
    m_h_loc_blurRadius = glGetUniformLocation(m_horizontalProgram, "uBlurRadius");
    m_h_loc_depthThreshold = glGetUniformLocation(m_horizontalProgram, "uDepthThreshold");
    m_h_loc_texelSize = glGetUniformLocation(m_horizontalProgram, "uTexelSize");

    // Compile vertical blur shader
    GLuint vs_v = CompileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs_v = CompileShader(GL_FRAGMENT_SHADER, kFS_Vertical);
    
    if (!vs_v || !fs_v) {
        if (vs_v) glDeleteShader(vs_v);
        if (fs_v) glDeleteShader(fs_v);
        return false;
    }

    m_verticalProgram = LinkProgram(vs_v, fs_v);
    glDeleteShader(vs_v);
    glDeleteShader(fs_v);

    if (!m_verticalProgram) {
        return false;
    }

    // Get uniform locations for vertical pass
    m_v_loc_inputTexture = glGetUniformLocation(m_verticalProgram, "uInputTexture");
    m_v_loc_depthTexture = glGetUniformLocation(m_verticalProgram, "uDepthTexture");
    m_v_loc_blurRadius = glGetUniformLocation(m_verticalProgram, "uBlurRadius");
    m_v_loc_depthThreshold = glGetUniformLocation(m_verticalProgram, "uDepthThreshold");
    m_v_loc_texelSize = glGetUniformLocation(m_verticalProgram, "uTexelSize");

    return true;
}

bool ShadowBlur::createFramebuffers() {
    // Horizontal pass framebuffer
    glGenFramebuffers(1, &m_horizontalFBO);
    glGenTextures(1, &m_horizontalTexture);

    glBindTexture(GL_TEXTURE_2D, m_horizontalTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, m_horizontalFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_horizontalTexture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[ShadowBlur] Horizontal framebuffer incomplete" << std::endl;
        return false;
    }

    // Vertical pass framebuffer
    glGenFramebuffers(1, &m_verticalFBO);
    glGenTextures(1, &m_verticalTexture);

    glBindTexture(GL_TEXTURE_2D, m_verticalTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, m_verticalFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_verticalTexture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[ShadowBlur] Vertical framebuffer incomplete" << std::endl;
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

void ShadowBlur::cleanup() {
    if (m_horizontalFBO) { glDeleteFramebuffers(1, &m_horizontalFBO); m_horizontalFBO = 0; }
    if (m_horizontalTexture) { glDeleteTextures(1, &m_horizontalTexture); m_horizontalTexture = 0; }
    if (m_verticalFBO) { glDeleteFramebuffers(1, &m_verticalFBO); m_verticalFBO = 0; }
    if (m_verticalTexture) { glDeleteTextures(1, &m_verticalTexture); m_verticalTexture = 0; }
}
