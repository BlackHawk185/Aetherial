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

// Smooth noise for waves (same as gbuffer shader)
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for (int i = 0; i < 4; i++) {
        value += amplitude * noise(p * frequency);
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

void main() {
    InstanceData inst = instances[gl_InstanceID];
    mat4 chunkTransform = transforms[inst.chunkDrawID];
    
    vec3 pos = inst.localPosition + aPosition;
    vec4 worldPos = chunkTransform * vec4(pos, 1.0);
    
    // Apply same wave displacement as forward shader for correct shadows
    if (aPosition.y > 0.4) {
        vec2 waveCoord = worldPos.xz * 0.1;
        float wave = fbm(waveCoord + vec2(uTime * 0.2, uTime * 0.15));
        float waveHeight = (wave - 0.5) * 1.5;
        worldPos.y += waveHeight;
    }
    
    gl_Position = uLightVP * worldPos;
}
