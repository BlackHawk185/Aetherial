#version 460 core

layout(location = 0) in vec3 aPosition;

struct InstanceData {
    vec3 localPosition;
    uint chunkDrawID;
};

layout(std430, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(std430, binding = 0) readonly buffer ChunkTransforms {
    mat4 transforms[];
};

uniform mat4 uLightVP;
uniform float uTime;

void main() {
    InstanceData inst = instances[gl_InstanceID];
    mat4 chunkTransform = transforms[inst.chunkDrawID];
    
    // Apply same wind animation as forward shader for correct shadow positioning
    float windStrength = 0.15;
    float heightFactor = max(0.0, aPosition.y * 0.8);
    
    float phase = fract(sin(dot(inst.localPosition.xz, vec2(127.1, 311.7))) * 43758.5453) * 6.28318;
    
    vec3 windOffset = vec3(
        sin(uTime * 3.6 + phase * 2.0) * windStrength * heightFactor,
        0.0,
        cos(uTime * 2.8 + phase * 1.7) * windStrength * heightFactor * 0.7
    );
    
    vec3 pos = inst.localPosition + aPosition + windOffset;
    vec4 worldPos = chunkTransform * vec4(pos, 1.0);
    gl_Position = uLightVP * worldPos;
}
