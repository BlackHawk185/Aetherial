#version 450

// Vertex attributes (from model)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

// Instance attributes
layout(location = 3) in vec3 instancePosition;
layout(location = 4) in uint instanceIslandID;
layout(location = 5) in uint instanceBlockID;

// Uniforms
layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec3 cameraPos;
    float time;
} pc;

layout(set = 0, binding = 0) readonly buffer IslandTransforms {
    mat4 transforms[];
} islandTransforms;

// Outputs
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec3 fragViewDir;
layout(location = 4) out flat uint fragBlockID;

const uint WATER_BLOCK_ID = 45;

// Simple 2D noise for natural variation
float noise2D(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Smooth noise interpolation
float smoothNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // Smoothstep
    
    float a = noise2D(i);
    float b = noise2D(i + vec2(1.0, 0.0));
    float c = noise2D(i + vec2(0.0, 1.0));
    float d = noise2D(i + vec2(1.0, 1.0));
    
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    mat4 islandTransform = islandTransforms.transforms[instanceIslandID];
    
    vec3 localPos = inPosition;
    vec3 normal = inNormal;
    
    // Gentle pool ripples for water
    if (instanceBlockID == WATER_BLOCK_ID) {
        vec3 worldPosForWaves = (islandTransform * vec4(inPosition + instancePosition, 1.0)).xyz;
        vec2 pos = worldPosForWaves.xz;
        
        float height = 0.0;
        float dx = 0.0;
        float dz = 0.0;
        
        // Slow-moving large-scale undulation (like gentle wind on pond)
        float t1 = pc.time * 0.3;
        float scale1 = 0.4;
        float amp1 = 0.012;
        height += (smoothNoise(pos * scale1 + vec2(t1 * 0.5, t1 * 0.3)) - 0.5) * amp1;
        
        // Medium frequency subtle ripples
        float t2 = pc.time * 0.6;
        float scale2 = 1.2;
        float amp2 = 0.008;
        height += (smoothNoise(pos * scale2 - vec2(t2 * 0.4, t2 * 0.7)) - 0.5) * amp2;
        
        // Fine detail (surface tension variation)
        float t3 = pc.time * 1.0;
        float scale3 = 2.5;
        float amp3 = 0.004;
        height += (smoothNoise(pos * scale3 + vec2(t3 * 0.6, -t3 * 0.5)) - 0.5) * amp3;
        
        // Calculate normal via finite difference (sample nearby points)
        float epsilon = 0.1;
        float hx = (smoothNoise((pos + vec2(epsilon, 0.0)) * scale1 + vec2(t1 * 0.5, t1 * 0.3)) - 0.5) * amp1
                 + (smoothNoise((pos + vec2(epsilon, 0.0)) * scale2 - vec2(t2 * 0.4, t2 * 0.7)) - 0.5) * amp2;
        float hz = (smoothNoise((pos + vec2(0.0, epsilon)) * scale1 + vec2(t1 * 0.5, t1 * 0.3)) - 0.5) * amp1
                 + (smoothNoise((pos + vec2(0.0, epsilon)) * scale2 - vec2(t2 * 0.4, t2 * 0.7)) - 0.5) * amp2;
        
        dx = (hx - height) / epsilon;
        dz = (hz - height) / epsilon;
        
        localPos.y += height;
        normal = normalize(vec3(-dx, 1.0, -dz));
    }
    
    vec4 worldPos = islandTransform * vec4(localPos + instancePosition, 1.0);
    fragWorldPos = worldPos.xyz;
    fragNormal = mat3(islandTransform) * normal;
    fragTexCoord = inTexCoord;
    fragViewDir = normalize(pc.cameraPos - fragWorldPos);
    fragBlockID = instanceBlockID;
    
    gl_Position = pc.viewProjection * worldPos;
}
