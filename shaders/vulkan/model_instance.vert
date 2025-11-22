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
    float time;
} pc;

layout(set = 0, binding = 0) readonly buffer IslandTransforms {
    mat4 transforms[];
} islandTransforms;

// Outputs
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec3 fragTangent;
layout(location = 4) out flat uint fragBlockID;

const uint WATER_BLOCK_ID = 45;
const uint GRASS_BLOCK_ID = 102;
const float WAVE_AMPLITUDE = 0.15;
const float WAVE_FREQUENCY = 1.5;
const float WAVE_SPEED = 0.8;

// Grass wind parameters
const float WIND_STRENGTH = 0.3;
const float WIND_SPEED = 1.2;
const float WIND_FREQUENCY = 0.8;

void main() {
    // Get island transform
    mat4 islandTransform = islandTransforms.transforms[instanceIslandID];
    
    vec3 localPos = inPosition;
    vec3 normal = inNormal;
    
    // Apply wave deformation for water blocks
    if (instanceBlockID == WATER_BLOCK_ID) {
        // World position for wave calculation
        vec3 worldPosForWaves = (islandTransform * vec4(inPosition + instancePosition, 1.0)).xyz;
        
        // Multiple overlapping sine waves
        float wave1 = sin(worldPosForWaves.x * WAVE_FREQUENCY + pc.time * WAVE_SPEED) * WAVE_AMPLITUDE;
        float wave2 = sin(worldPosForWaves.z * WAVE_FREQUENCY * 1.3 + pc.time * WAVE_SPEED * 0.7) * WAVE_AMPLITUDE * 0.8;
        float wave3 = sin((worldPosForWaves.x + worldPosForWaves.z) * WAVE_FREQUENCY * 0.5 + pc.time * WAVE_SPEED * 1.5) * WAVE_AMPLITUDE * 0.6;
        
        // Apply wave displacement to Y coordinate
        localPos.y += wave1 + wave2 + wave3;
        
        // Perturb normal based on wave gradients for lighting
        float dx = cos(worldPosForWaves.x * WAVE_FREQUENCY + pc.time * WAVE_SPEED) * WAVE_FREQUENCY * WAVE_AMPLITUDE;
        float dz = cos(worldPosForWaves.z * WAVE_FREQUENCY * 1.3 + pc.time * WAVE_SPEED * 0.7) * WAVE_FREQUENCY * 1.3 * WAVE_AMPLITUDE * 0.8;
        
        normal = normalize(vec3(-dx, 1.0, -dz));
    }
    // Apply wind deformation for grass blocks
    else if (instanceBlockID == GRASS_BLOCK_ID) {
        vec3 worldPosForWind = (islandTransform * vec4(inPosition + instancePosition, 1.0)).xyz;
        
        // Height-based wind influence (stronger at top)
        float heightFactor = clamp(inPosition.y, 0.0, 1.0);
        
        // Multi-directional wind waves
        float wind1 = sin(worldPosForWind.x * WIND_FREQUENCY + pc.time * WIND_SPEED) * WIND_STRENGTH;
        float wind2 = sin(worldPosForWind.z * WIND_FREQUENCY * 0.7 + pc.time * WIND_SPEED * 1.3) * WIND_STRENGTH * 0.7;
        float wind3 = sin((worldPosForWind.x + worldPosForWind.z) * WIND_FREQUENCY * 0.5 + pc.time * WIND_SPEED * 0.8) * WIND_STRENGTH * 0.5;
        
        float totalWind = (wind1 + wind2 + wind3) * heightFactor;
        
        // Apply horizontal displacement
        localPos.x += totalWind * 0.6;
        localPos.z += totalWind * 0.4;
        
        // Slight vertical bob
        localPos.y += abs(totalWind) * 0.1;
    }
    
    // Transform vertex to world space
    vec4 worldPos = islandTransform * vec4(localPos + instancePosition, 1.0);
    fragWorldPos = worldPos.xyz;
    
    // Transform normal
    fragNormal = mat3(islandTransform) * normal;
    
    // Calculate tangent for reflectance
    fragTangent = vec3(1.0, 0.0, 0.0);
    
    // Pass through texture coordinates
    fragTexCoord = inTexCoord;
    
    // Pass through block ID
    fragBlockID = instanceBlockID;
    
    // Project to clip space
    gl_Position = pc.viewProjection * worldPos;
}
