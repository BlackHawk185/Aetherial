#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// Instance data from SSBO
struct InstanceData {
    vec3 localPosition;
    uint chunkDrawID;
};

layout(std430, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

// Chunk transforms (shared with InstancedQuadRenderer)
layout(std430, binding = 0) readonly buffer ChunkTransforms {
    mat4 transforms[];
};

uniform mat4 uViewProjection;
uniform float uTime;

out vec2 vUV;
out vec3 vNormal;
out vec3 vWorldPos;

void main() {
    InstanceData inst = instances[gl_InstanceID];
    mat4 chunkTransform = transforms[inst.chunkDrawID];
    
    // Wind sway: affect vertices based on their height within the grass model
    // Higher vertices (larger Y) sway more, creating natural grass movement
    float windStrength = 0.15;
    float heightFactor = max(0.0, aPosition.y * 0.8); // Scale with vertex height
    
    // Generate phase from local position for deterministic variation
    float phase = fract(sin(dot(inst.localPosition.xz, vec2(127.1, 311.7))) * 43758.5453) * 6.28318;
    
    vec3 windOffset = vec3(
        sin(uTime * 3.6 + phase * 2.0) * windStrength * heightFactor,
        0.0,
        cos(uTime * 2.8 + phase * 1.7) * windStrength * heightFactor * 0.7
    );
    
    // Local position within chunk + vertex offset + wind
    vec3 pos = inst.localPosition + aPosition + windOffset;
    vec4 worldPos = chunkTransform * vec4(pos, 1.0);
    
    gl_Position = uViewProjection * worldPos;
    vUV = aUV;
    vNormal = mat3(chunkTransform) * aNormal;
    vWorldPos = worldPos.xyz;
}
