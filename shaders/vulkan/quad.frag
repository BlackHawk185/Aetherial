#version 450

// Inputs from vertex shader (quad_vertex_pulling.vert)
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in flat uint vBlockType;
layout(location = 3) in vec3 vWorldPos;

// G-buffer outputs (3 attachments - matches pipeline)
layout(location = 0) out vec4 gAlbedo;   // RGB = base color, A = AO
layout(location = 1) out vec4 gNormal;   // RGB = normal (encoded), A = unused
layout(location = 2) out vec4 gPosition; // RGB = world position, A = unused

// Simple block colors (temporary - will use texture atlas later)
vec3 getBlockColor(uint blockType) {
    switch (blockType) {
        case 0u:  return vec3(0.0);              // Air (shouldn't render)
        case 1u:  return vec3(0.5, 0.5, 0.5);    // Stone (gray)
        case 2u:  return vec3(0.4, 0.25, 0.1);   // Dirt (brown)
        case 3u:  return vec3(0.2, 0.7, 0.2);    // Grass (green)
        case 4u:  return vec3(0.8, 0.7, 0.4);    // Sand (tan)
        case 5u:  return vec3(0.6, 0.3, 0.1);    // Wood (brown)
        case 6u:  return vec3(0.3, 0.6, 0.3);    // Leaves (green)
        case 45u: return vec3(0.2, 0.4, 0.8);    // Water (blue)
        default:  return vec3(1.0, 0.0, 1.0);    // Magenta (unknown)
    }
}

void main() {
    // Get base color from block type
    vec3 albedo = getBlockColor(vBlockType);
    
    // Normalize normal (in case vertex shader didn't)
    vec3 normal = normalize(vNormal);
    
    // Calculate face direction from normal for AO
    vec3 absNormal = abs(normal);
    uint faceDir = 0u;
    if (absNormal.x > absNormal.y && absNormal.x > absNormal.z) {
        faceDir = (normal.x > 0.0) ? 1u : 0u;  // +X or -X
    } else if (absNormal.y > absNormal.z) {
        faceDir = (normal.y > 0.0) ? 3u : 2u;  // +Y or -Y
    } else {
        faceDir = (normal.z > 0.0) ? 5u : 4u;  // +Z or -Z
    }
    
    // Simple AO based on face direction
    float ao = 1.0;
    if (faceDir == 2u) {
        ao = 0.6;  // Bottom face (-Y)
    } else if (faceDir == 3u) {
        ao = 1.0;  // Top face (+Y)
    } else {
        ao = 0.85; // Side faces
    }
    
    // Write to G-buffer
    gAlbedo = vec4(albedo, ao);
    gNormal = vec4(normal * 0.5 + 0.5, 1.0);  // Encode [-1,1] to [0,1]
    gPosition = vec4(vWorldPos, 1.0);
}
