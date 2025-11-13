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

// Smooth noise for waves
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);  // Quintic interpolation

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
    
    // Local position within chunk + vertex offset
    vec3 pos = inst.localPosition + aPosition;
    vec4 worldPos = chunkTransform * vec4(pos, 1.0);
    
    float waveHeight = 0.0;
    vec3 normal = aNormal;
    
    // Only displace vertices on top surface (y > 0.4 in model space)
    if (aPosition.y > 0.4) {
        vec2 waveCoord = worldPos.xz * 0.1;  // More visible waves
        float wave = fbm(waveCoord + vec2(uTime * 0.2, uTime * 0.15));
        waveHeight = (wave - 0.5) * 1.5;  // Exaggerated wave displacement
        worldPos.y += waveHeight;
        
        // Compute normal from wave displacement for proper lighting
        float h = 0.1;
        float heightR = fbm((worldPos.xz + vec2(h, 0.0)) * 0.1 + vec2(uTime * 0.2, uTime * 0.15));
        float heightU = fbm((worldPos.xz + vec2(0.0, h)) * 0.1 + vec2(uTime * 0.2, uTime * 0.15));
        
        vec3 tangentX = vec3(h, (heightR - wave) * 1.5, 0.0);
        vec3 tangentZ = vec3(0.0, (heightU - wave) * 1.5, h);
        normal = normalize(cross(tangentZ, tangentX));
    }
    
    gl_Position = uViewProjection * worldPos;
    vUV = aUV;
    vNormal = mat3(chunkTransform) * normal;
    vWorldPos = worldPos.xyz;
}
