#version 450
#extension GL_ARB_shader_draw_parameters : require

// Vertex attributes (QuadVertex)
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec3 aNormal;

// Instance attributes (QuadFace)
layout(location = 3) in vec3 aQuadPosition;
layout(location = 4) in vec3 aQuadNormal;
layout(location = 5) in vec4 aQuadTangent;
layout(location = 6) in vec2 aQuadSize;
layout(location = 7) in vec4 aQuadTexCoord;
layout(location = 8) in uint aQuadBlockType;
layout(location = 9) in uint aQuadFaceDir;

// Transform SSBO
layout(set = 0, binding = 0) readonly buffer ChunkTransforms {
    mat4 transforms[];
};

// Push constants (camera matrices)
layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

// Outputs to fragment shader
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;
layout(location = 3) out flat uint vBlockType;
layout(location = 4) out flat uint vFaceDir;

void main() {
    // Get chunk transform from SSBO using gl_DrawIDARB (MDI)
    mat4 chunkTransform = transforms[gl_DrawIDARB];
    
    // Build quad-space position
    vec3 quadPos = aPosition * vec3(aQuadSize, 1.0);
    
    // Transform to chunk space using quad's TBN
    vec3 tangent = aQuadTangent.xyz;
    vec3 bitangent = cross(aQuadNormal, tangent) * aQuadTangent.w;
    mat3 tbn = mat3(tangent, bitangent, aQuadNormal);
    vec3 chunkPos = aQuadPosition + tbn * quadPos;
    
    // Transform to world space
    vec4 worldPos = chunkTransform * vec4(chunkPos, 1.0);
    vWorldPos = worldPos.xyz;
    
    // Transform normal to world space
    mat3 normalMatrix = mat3(chunkTransform);
    vNormal = normalize(normalMatrix * aQuadNormal);
    
    // Texture coordinates
    vTexCoord = aQuadTexCoord.xy + aTexCoord * aQuadTexCoord.zw;
    
    // Metadata
    vBlockType = aQuadBlockType;
    vFaceDir = aQuadFaceDir;
    
    // Final position
    gl_Position = pc.viewProj * worldPos;
}
