#version 460 core
layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 sunDir;
    float sunIntensity;
    vec3 moonDir;
    float moonIntensity;
    vec3 cameraPos;
    float timeOfDay;
    float sunSize;
    float sunGlow;
    float moonSize;
    float exposure;
} pc;

layout(location = 0) out vec3 vWorldPos;

void main() {
    vWorldPos = aPos;
    
    // Transform by view-projection (translation already removed in viewProj)
    vec4 pos = pc.viewProj * vec4(aPos, 1.0);
    
    // Set z to w to ensure skybox is at far plane after perspective divide
    gl_Position = pos.xyww;
}
