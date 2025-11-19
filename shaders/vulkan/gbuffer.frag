#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in flat uint vBlockType;
layout(location = 4) in flat uint vFaceDir;

// G-buffer outputs (Multiple Render Targets)
layout(location = 0) out vec4 gAlbedo;     // RGB16F - base color
layout(location = 1) out vec4 gNormal;     // RGB16F - world-space normal
layout(location = 2) out vec4 gPosition;   // RGB32F - world position
layout(location = 3) out vec4 gMetadata;   // RGBA8 - block type, face dir

// Texture sampler (block texture atlas)
layout(set = 1, binding = 0) uniform sampler2D uBlockAtlas;

void main() {
    // Sample albedo from texture atlas
    vec4 albedo = texture(uBlockAtlas, vTexCoord);
    
    // Discard fully transparent fragments (for foliage, etc.)
    if (albedo.a < 0.1) {
        discard;
    }
    
    // Output to G-buffer
    gAlbedo = vec4(albedo.rgb, 1.0);
    gNormal = vec4(normalize(vNormal), 0.0);
    gPosition = vec4(vWorldPos, 1.0);
    
    // Pack metadata (block type in R, face direction in G)
    gMetadata = vec4(
        float(vBlockType) / 255.0,
        float(vFaceDir) / 255.0,
        0.0,
        1.0
    );
}
