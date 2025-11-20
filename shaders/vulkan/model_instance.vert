#version 450

// Vertex attributes (from model)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

// Instance attributes
layout(location = 3) in vec3 instancePosition;
layout(location = 4) in uint instanceIslandID;

// Uniforms
layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
} pc;

layout(set = 0, binding = 0) readonly buffer IslandTransforms {
    mat4 transforms[];
} islandTransforms;

// Outputs
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;

void main() {
    // Get island transform
    mat4 islandTransform = islandTransforms.transforms[instanceIslandID];
    
    // Transform vertex to world space
    vec4 worldPos = islandTransform * vec4(inPosition + instancePosition, 1.0);
    fragWorldPos = worldPos.xyz;
    
    // Transform normal (assumes uniform scaling)
    fragNormal = mat3(islandTransform) * inNormal;
    
    // Pass through texture coordinates
    fragTexCoord = inTexCoord;
    
    // Project to clip space
    gl_Position = pc.viewProjection * worldPos;
}
