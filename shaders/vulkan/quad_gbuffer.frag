#version 450

// Inputs from vertex shader (quad_vertex_pulling.vert)
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in flat uint vBlockType;
layout(location = 3) in vec3 vWorldPos;

// G-buffer outputs
layout(location = 0) out vec4 gAlbedo;      // RGB = base color, A = AO
layout(location = 1) out vec4 gNormal;      // RGB = normal (encoded), A = roughness
layout(location = 2) out vec4 gWorldPos;    // RGB = world position, A = unused
layout(location = 3) out vec4 gMetadata;    // R8G8B8A8_UNORM format

// Texture array sampler
layout(set = 0, binding = 0) uniform sampler2DArray uBlockTextures;

void main() {
    // Sample texture from array (blockType = layer index)
    vec3 albedo = texture(uBlockTextures, vec3(vTexCoord, float(vBlockType))).rgb;
    
    // Normalize normal (in case vertex shader didn't)
    vec3 normal = normalize(vNormal);
    
    // Calculate face direction from normal for AO
    // -X = 0, +X = 1, -Y = 2, +Y = 3, -Z = 4, +Z = 5
    vec3 absNormal = abs(normal);
    uint faceDir = 0u;
    if (absNormal.x > absNormal.y && absNormal.x > absNormal.z) {
        faceDir = (normal.x > 0.0) ? 1u : 0u;  // +X or -X
    } else if (absNormal.y > absNormal.z) {
        faceDir = (normal.y > 0.0) ? 3u : 2u;  // +Y or -Y
    } else {
        faceDir = (normal.z > 0.0) ? 5u : 4u;  // +Z or -Z
    }
    
    // Simple AO based on face direction (top faces lighter, bottom darker)
    float ao = 1.0;
    if (faceDir == 2u) {
        ao = 0.6;  // Bottom face (-Y)
    } else if (faceDir == 3u) {
        ao = 1.0;  // Top face (+Y)
    } else {
        ao = 0.85; // Side faces
    }
    
    // Material properties (blocks are rough, not shiny)
    float roughness = 0.8;
    
    // Write to G-buffer
    gAlbedo = vec4(albedo, ao);
    gNormal = vec4(normal * 0.5 + 0.5, roughness);  // Encode normal to [0,1]
    gWorldPos = vec4(vWorldPos, 1.0);
    // Pack metadata into RGBA8 (normalized 0-1 range)
    gMetadata = vec4(float(vBlockType) / 255.0, float(faceDir) / 255.0, 0.0, 1.0);
}
