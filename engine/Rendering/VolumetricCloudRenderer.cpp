#include "VolumetricCloudRenderer.h"
#include "Parameters.h"
#include <glad/gl.h>
#include <string>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <filesystem>

// Global cloud renderer instance
VolumetricCloudRenderer g_cloudRenderer;

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

    // Cloud raymarching fragment shader - dynamically generated with parameters
    std::string cloudFS = R"GLSL(
#version 460 core
in vec2 vUV;

uniform mat4 uViewMatrix;
uniform mat4 uProjectionMatrix;
uniform mat4 uInvProjectionMatrix;
uniform mat4 uInvViewMatrix;
uniform vec3 uCameraPosition;
uniform vec3 uSunDirection;
uniform float uSunIntensity;
uniform float uTimeOfDay;
uniform float uCloudCoverage;
uniform float uCloudDensity;
uniform float uCloudSpeed;
uniform sampler2D uDepthTexture;
uniform sampler3D uNoiseTexture;

out vec4 FragColor;

// Cloud volume bounds - 3D noise naturally creates varied height clouds
const float CLOUD_BASE_MIN = )GLSL" + std::to_string(EngineParameters::Clouds::CLOUD_BASE_MIN_HEIGHT) + R"GLSL(;
const float CLOUD_BASE_MAX = )GLSL" + std::to_string(EngineParameters::Clouds::CLOUD_BASE_MAX_HEIGHT) + R"GLSL(;

// Cloud appearance parameters (injected from EngineParameters)
const float CLOUD_SCALE = )GLSL" + std::to_string(EngineParameters::Clouds::CLOUD_SCALE) + R"GLSL(;

// Raymarching parameters
const int MAX_STEPS = )GLSL" + std::to_string(EngineParameters::Clouds::RAYMARCH_SAMPLES) + R"GLSL(;
const float MAX_DISTANCE = 1000.0;

// Reconstruct world position from depth
vec3 worldPositionFromDepth(vec2 uv, float depth) {
    vec4 clipSpace = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewSpace = uInvProjectionMatrix * clipSpace;
    viewSpace /= viewSpace.w;
    vec4 worldSpace = uInvViewMatrix * viewSpace;
    return worldSpace.xyz;
}

// Sample cloud density from 3D noise - Y-axis gives natural height variation
float sampleCloudDensity(vec3 position, float time) {
    // Check if we're in the cloud volume at all
    if (position.y < CLOUD_BASE_MIN || position.y > CLOUD_BASE_MAX) {
        return 0.0;
    }
    
    // Apply wind offset (move clouds over time)
    vec3 windOffset = vec3(time * uCloudSpeed * 0.05, 0.0, time * uCloudSpeed * 0.03);
    vec3 samplePos = (position + windOffset) * CLOUD_SCALE;
    
    // Multi-octave 3D noise sampling with offset per octave to break tiling
    float noise = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    float maxValue = 0.0;
    
    // Different offsets per octave to eliminate any tiling artifacts
    vec3 octaveOffsets[4] = vec3[4](
        vec3(0.0, 0.0, 0.0),
        vec3(123.456, 789.012, 345.678),
        vec3(901.234, 567.890, 123.456),
        vec3(456.789, 234.567, 890.123)
    );
    
    for (int i = 0; i < 4; i++) {
        vec3 offsetPos = samplePos * frequency + octaveOffsets[i];
        noise += texture(uNoiseTexture, offsetPos).r * amplitude;
        maxValue += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    
    noise /= maxValue;
    
    // Apply coverage and remap
    float density = max(0.0, noise - (1.0 - uCloudCoverage)) * uCloudDensity;
    
    // Height-based density falloff at volume edges (top and bottom)
    float heightInVolume = position.y - CLOUD_BASE_MIN;
    float volumeHeight = CLOUD_BASE_MAX - CLOUD_BASE_MIN;
    float heightGradient = smoothstep(0.0, 30.0, heightInVolume) * 
                          smoothstep(volumeHeight, volumeHeight - 30.0, heightInVolume);
    
    return density * heightGradient;
}

// Simple light scattering
float lightEnergy(vec3 position, float time) {
    // Sample in sun direction for better scattering
    vec3 lightSamplePos = position + uSunDirection * 30.0;
    float density = sampleCloudDensity(lightSamplePos, time);
    
    // Beer-Lambert law
    return exp(-density * 2.0);
}

// Ray-slab intersection for all cloud layers
bool intersectCloudLayer(vec3 origin, vec3 direction, out float tMin, out float tMax) {
    // Check intersection with expanded cloud volume covering all layers
    float t1 = (CLOUD_BASE_MIN - origin.y) / direction.y;
    float t2 = (CLOUD_BASE_MAX - origin.y) / direction.y;
    
    tMin = min(t1, t2);
    tMax = max(t1, t2);
    
    // Clamp to forward ray
    tMin = max(tMin, 0.0);
    
    return tMax > tMin;
}

void main() {
    // Get scene depth
    float sceneDepth = texture(uDepthTexture, vUV).r;
    vec3 sceneWorldPos = worldPositionFromDepth(vUV, sceneDepth);
    float sceneDistance = length(sceneWorldPos - uCameraPosition);
    
    // Reconstruct ray direction
    vec3 rayDir = normalize(sceneWorldPos - uCameraPosition);
    
    // Find intersection with cloud layer
    float tMin, tMax;
    if (!intersectCloudLayer(uCameraPosition, rayDir, tMin, tMax)) {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    
    // Clamp to scene depth
    tMax = min(tMax, sceneDistance);
    
    if (tMax <= tMin) {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    
    // Raymarch through cloud layer
    float stepSize = (tMax - tMin) / float(MAX_STEPS);
    float t = tMin;
    
    float transmittance = 1.0;
    vec3 cloudColor = vec3(0.0);
    
    for (int i = 0; i < MAX_STEPS; i++) {
        if (transmittance < 0.01) break;
        
        vec3 samplePos = uCameraPosition + rayDir * t;
        float density = sampleCloudDensity(samplePos, uTimeOfDay);
        
        if (density > 0.001) {
            float light = lightEnergy(samplePos, uTimeOfDay);
            
            // Sun color - warm white
            vec3 sunColor = vec3(1.0, 0.95, 0.85) * uSunIntensity;
            
            // Ambient sky color
            vec3 ambientColor = vec3(0.5, 0.6, 0.7) * 0.3;
            
            // Combine lighting
            vec3 lighting = sunColor * light + ambientColor;
            
            // Accumulate color
            float densityStep = density * stepSize;
            cloudColor += transmittance * lighting * densityStep;
            transmittance *= exp(-densityStep);
        }
        
        t += stepSize;
        if (t >= tMax) break;
    }
    
    float alpha = 1.0 - transmittance;
    FragColor = vec4(cloudColor, alpha);
}
)GLSL";
    
    const char* kFS = cloudFS.c_str();

    // Hash function for noise generation
    float hash(float n) {
        return glm::fract(sin(n) * 43758.5453123f);
    }

    // 3D hash for Worley noise
    glm::vec3 hash3(glm::vec3 p) {
        p = glm::vec3(glm::dot(p, glm::vec3(127.1f, 311.7f, 74.7f)),
                      glm::dot(p, glm::vec3(269.5f, 183.3f, 246.1f)),
                      glm::dot(p, glm::vec3(113.5f, 271.9f, 124.6f)));
        return glm::fract(glm::sin(p) * 43758.5453123f);
    }
}

VolumetricCloudRenderer::VolumetricCloudRenderer() = default;

VolumetricCloudRenderer::~VolumetricCloudRenderer() {
    shutdown();
}

bool VolumetricCloudRenderer::initialize() {
    if (!createShaders()) return false;
    if (!createGeometry()) return false;
    if (!create3DNoiseTexture()) return false;
    
    std::cout << "VolumetricCloudRenderer initialized successfully" << std::endl;
    return true;
}

void VolumetricCloudRenderer::shutdown() {
    if (m_shader) glDeleteProgram(m_shader);
    if (m_quadVAO) glDeleteVertexArrays(1, &m_quadVAO);
    if (m_quadVBO) glDeleteBuffers(1, &m_quadVBO);
    if (m_noiseTexture3D) glDeleteTextures(1, &m_noiseTexture3D);
    
    m_shader = 0;
    m_quadVAO = 0;
    m_quadVBO = 0;
    m_noiseTexture3D = 0;
}

bool VolumetricCloudRenderer::createShaders() {
    // Compile vertex shader
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kVS, nullptr);
    glCompileShader(vs);
    
    GLint success;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(vs, 512, nullptr, log);
        std::cerr << "Cloud vertex shader compilation failed:\n" << log << std::endl;
        glDeleteShader(vs);
        return false;
    }
    
    // Compile fragment shader
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kFS, nullptr);
    glCompileShader(fs);
    
    glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(fs, 512, nullptr, log);
        std::cerr << "Cloud fragment shader compilation failed:\n" << log << std::endl;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    
    // Link program
    m_shader = glCreateProgram();
    glAttachShader(m_shader, vs);
    glAttachShader(m_shader, fs);
    glLinkProgram(m_shader);
    
    glGetProgramiv(m_shader, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(m_shader, 512, nullptr, log);
        std::cerr << "Cloud shader linking failed:\n" << log << std::endl;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    // Get uniform locations
    m_uViewMatrix = glGetUniformLocation(m_shader, "uViewMatrix");
    m_uProjectionMatrix = glGetUniformLocation(m_shader, "uProjectionMatrix");
    m_uInvProjectionMatrix = glGetUniformLocation(m_shader, "uInvProjectionMatrix");
    m_uInvViewMatrix = glGetUniformLocation(m_shader, "uInvViewMatrix");
    m_uCameraPosition = glGetUniformLocation(m_shader, "uCameraPosition");
    m_uSunDirection = glGetUniformLocation(m_shader, "uSunDirection");
    m_uSunIntensity = glGetUniformLocation(m_shader, "uSunIntensity");
    m_uTimeOfDay = glGetUniformLocation(m_shader, "uTimeOfDay");
    m_uCloudCoverage = glGetUniformLocation(m_shader, "uCloudCoverage");
    m_uCloudDensity = glGetUniformLocation(m_shader, "uCloudDensity");
    m_uCloudSpeed = glGetUniformLocation(m_shader, "uCloudSpeed");
    m_uDepthTexture = glGetUniformLocation(m_shader, "uDepthTexture");
    m_uNoiseTexture = glGetUniformLocation(m_shader, "uNoiseTexture");
    
    return true;
}

bool VolumetricCloudRenderer::createGeometry() {
    // Fullscreen quad vertices
    float quadVertices[] = {
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
    
    return true;
}

bool VolumetricCloudRenderer::create3DNoiseTexture() {
    const int size = EngineParameters::Clouds::NOISE_TEXTURE_SIZE;
    std::vector<unsigned char> noiseData(size * size * size);
    
    std::cout << "Generating " << size << "^3 cloud noise texture..." << std::endl;
    
    // Generate 3D Perlin-Worley noise
    for (int z = 0; z < size; z++) {
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                float nx = float(x) / float(size);
                float ny = float(y) / float(size);
                float nz = float(z) / float(size);
                
                // Perlin noise component
                float perlin = perlinNoise3D(nx * 4.0f, ny * 4.0f, nz * 4.0f);
                
                // Worley noise component
                float worley = worleyNoise3D(nx * 2.0f, ny * 2.0f, nz * 2.0f);
                
                // Combine (Perlin-Worley hybrid)
                float noise = perlin * 0.6f + worley * 0.4f;
                noise = std::clamp(noise, 0.0f, 1.0f);
                
                int index = x + y * size + z * size * size;
                noiseData[index] = static_cast<unsigned char>(noise * 255.0f);
            }
        }
    }
    
    // Create 3D texture
    glGenTextures(1, &m_noiseTexture3D);
    glBindTexture(GL_TEXTURE_3D, m_noiseTexture3D);
    
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, size, size, size, 0, GL_RED, GL_UNSIGNED_BYTE, noiseData.data());
    
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT);
    
    glBindTexture(GL_TEXTURE_3D, 0);
    
    std::cout << "Cloud noise texture generated successfully" << std::endl;
    return true;
}

float VolumetricCloudRenderer::perlinNoise3D(float x, float y, float z) {
    // Simplified 3D Perlin noise
    int xi = static_cast<int>(std::floor(x)) & 255;
    int yi = static_cast<int>(std::floor(y)) & 255;
    int zi = static_cast<int>(std::floor(z)) & 255;
    
    float xf = x - std::floor(x);
    float yf = y - std::floor(y);
    float zf = z - std::floor(z);
    
    // Smoothstep interpolation
    float u = xf * xf * (3.0f - 2.0f * xf);
    float v = yf * yf * (3.0f - 2.0f * yf);
    float w = zf * zf * (3.0f - 2.0f * zf);
    
    // Hash-based gradients
    float n000 = hash(float(xi + yi * 57 + zi * 113));
    float n001 = hash(float(xi + yi * 57 + (zi + 1) * 113));
    float n010 = hash(float(xi + (yi + 1) * 57 + zi * 113));
    float n011 = hash(float(xi + (yi + 1) * 57 + (zi + 1) * 113));
    float n100 = hash(float((xi + 1) + yi * 57 + zi * 113));
    float n101 = hash(float((xi + 1) + yi * 57 + (zi + 1) * 113));
    float n110 = hash(float((xi + 1) + (yi + 1) * 57 + zi * 113));
    float n111 = hash(float((xi + 1) + (yi + 1) * 57 + (zi + 1) * 113));
    
    // Trilinear interpolation
    float nx00 = glm::mix(n000, n100, u);
    float nx01 = glm::mix(n001, n101, u);
    float nx10 = glm::mix(n010, n110, u);
    float nx11 = glm::mix(n011, n111, u);
    
    float nxy0 = glm::mix(nx00, nx10, v);
    float nxy1 = glm::mix(nx01, nx11, v);
    
    return glm::mix(nxy0, nxy1, w);
}

float VolumetricCloudRenderer::worleyNoise3D(float x, float y, float z) {
    // Simplified Worley (cellular) noise
    glm::vec3 point(x, y, z);
    glm::vec3 cell = glm::floor(point);
    glm::vec3 frac = glm::fract(point);
    
    float minDist = 1.0f;
    
    // Check neighboring cells
    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                glm::vec3 neighbor = cell + glm::vec3(dx, dy, dz);
                glm::vec3 feature = hash3(neighbor);
                glm::vec3 diff = (neighbor + feature) - point;
                float dist = glm::length(diff);
                minDist = std::min(minDist, dist);
            }
        }
    }
    
    return 1.0f - minDist; // Invert so higher = more solid
}

void VolumetricCloudRenderer::render(const glm::vec3& sunDirection, float sunIntensity,
                                     const glm::vec3& cameraPosition,
                                     const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                                     GLuint depthTexture, float timeOfDay) {
    if (!EngineParameters::Clouds::ENABLE_CLOUDS) return;
    
    // Early out if camera is far outside cloud layer (optimization)
    const float cloudLayerThickness = EngineParameters::Clouds::CLOUD_BASE_MAX_HEIGHT - EngineParameters::Clouds::CLOUD_BASE_MIN_HEIGHT;
    const float cullDistance = cloudLayerThickness * 2.0f;  // 2x layer thickness
    if (cameraPosition.y < EngineParameters::Clouds::CLOUD_BASE_MIN_HEIGHT - cullDistance ||
        cameraPosition.y > EngineParameters::Clouds::CLOUD_BASE_MAX_HEIGHT + cullDistance) {
        return;  // Clouds not visible from this altitude
    }
    
    glUseProgram(m_shader);
    
    updateUniforms(sunDirection, sunIntensity, cameraPosition, viewMatrix, projectionMatrix, timeOfDay);
    
    // Bind depth texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depthTexture);
    glUniform1i(m_uDepthTexture, 0);
    
    // Bind 3D noise texture
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, m_noiseTexture3D);
    glUniform1i(m_uNoiseTexture, 1);
    
    // Enable blending for clouds
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE); // Don't write to depth buffer
    
    // Render fullscreen quad
    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    
    // Restore state
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    
    glUseProgram(0);
}

float VolumetricCloudRenderer::sampleCloudDensityAt(const glm::vec3& worldPosition, float timeOfDay) {
    // This mirrors the shader logic but runs on CPU for shadow map integration
    using namespace EngineParameters::Clouds;
    
    // Check if we're in the cloud volume at all
    if (worldPosition.y < CLOUD_BASE_MIN_HEIGHT || worldPosition.y > CLOUD_BASE_MAX_HEIGHT) {
        return 0.0f;
    }
    
    // Apply wind offset
    glm::vec3 windOffset = glm::vec3(timeOfDay * CLOUD_SPEED * 0.05f, 0.0f, timeOfDay * CLOUD_SPEED * 0.03f);
    glm::vec3 samplePos = (worldPosition + windOffset) * CLOUD_SCALE;
    
    // Sample 3D noise (Perlin for CPU efficiency - Y-axis naturally creates height variation)
    float noise = perlinNoise3D(samplePos.x, samplePos.y, samplePos.z);
    
    // Apply coverage
    float density = std::max(0.0f, noise - (1.0f - CLOUD_COVERAGE)) * CLOUD_DENSITY;
    
    // Height gradient at volume edges
    float heightInVolume = worldPosition.y - CLOUD_BASE_MIN_HEIGHT;
    float volumeHeight = CLOUD_BASE_MAX_HEIGHT - CLOUD_BASE_MIN_HEIGHT;
    float t1 = glm::clamp(heightInVolume / 30.0f, 0.0f, 1.0f);
    float t2 = glm::clamp((volumeHeight - heightInVolume) / 30.0f, 0.0f, 1.0f);
    float heightGradient = t1 * t2;
    
    return density * heightGradient;
}

void VolumetricCloudRenderer::updateUniforms(const glm::vec3& sunDirection, float sunIntensity,
                                             const glm::vec3& cameraPosition,
                                             const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                                             float timeOfDay) {
    glm::mat4 invProjection = glm::inverse(projectionMatrix);
    glm::mat4 invView = glm::inverse(viewMatrix);
    
    glUniformMatrix4fv(m_uViewMatrix, 1, GL_FALSE, glm::value_ptr(viewMatrix));
    glUniformMatrix4fv(m_uProjectionMatrix, 1, GL_FALSE, glm::value_ptr(projectionMatrix));
    glUniformMatrix4fv(m_uInvProjectionMatrix, 1, GL_FALSE, glm::value_ptr(invProjection));
    glUniformMatrix4fv(m_uInvViewMatrix, 1, GL_FALSE, glm::value_ptr(invView));
    glUniform3fv(m_uCameraPosition, 1, glm::value_ptr(cameraPosition));
    glUniform3fv(m_uSunDirection, 1, glm::value_ptr(sunDirection));
    glUniform1f(m_uSunIntensity, sunIntensity);
    glUniform1f(m_uTimeOfDay, timeOfDay);
    glUniform1f(m_uCloudCoverage, m_cloudCoverage);
    glUniform1f(m_uCloudDensity, m_cloudDensity);
    glUniform1f(m_uCloudSpeed, m_cloudSpeed);
}
