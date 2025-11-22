#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec3 fragTangent;
layout(location = 4) in flat uint fragBlockID;

// G-Buffer outputs (match VulkanGBuffer.cpp format)
layout(location = 0) out vec4 outAlbedo;      // RGB: albedo, A: ao
layout(location = 1) out vec4 outNormal;      // RGB: world normal, A: roughness
layout(location = 2) out vec4 outPosition;    // RGB: world position, A: unused
layout(location = 3) out vec4 outMetadata;    // R: metallic, G: material flag, BA: unused

const uint WATER_BLOCK_ID = 45;
const uint GRASS_BLOCK_ID = 102;

void main() {
    vec3 albedo;
    float roughness;
    float metallic;
    float ao = 1.0;
    
    // Normalize interpolated normal
    vec3 normal = normalize(fragNormal);
    
    // Check if this is a water block
    if (fragBlockID == WATER_BLOCK_ID) {
        // Water material properties with wave reflections
        albedo = vec3(0.05, 0.15, 0.25);  // Dark blue-green water
        roughness = 0.08;  // Very smooth for reflections
        metallic = 0.15;   // Low metallic - water is dielectric
        ao = 0.9;          // Transparency (alpha)
        
        // Keep vertex shader wave normals for proper reflections
        // normal already set from vertex shader with wave deformation
        
        // Water gets special treatment in lighting pass via metadata
        // The lighting shader detects water and applies SSPR
    } 
    // Grass material
    else if (fragBlockID == GRASS_BLOCK_ID) {
        // Rich green grass color
        vec3 baseGreen = vec3(0.2, 0.6, 0.15);
        vec3 tipGreen = vec3(0.4, 0.8, 0.3);
        
        // Vary color based on height (UV.y or world Y)
        float heightBlend = clamp(fragTexCoord.y, 0.0, 1.0);
        albedo = mix(baseGreen, tipGreen, heightBlend);
        
        // Add slight color variation
        albedo += vec3(sin(fragWorldPos.x * 10.0) * 0.05, 
                       cos(fragWorldPos.z * 10.0) * 0.05, 
                       0.0);
        
        roughness = 0.7;   // Fairly rough surface
        metallic = 0.0;    // Non-metallic
        ao = 0.9;          // Slight ambient occlusion
    }
    else {
        // Magenta fallback for other materials
        albedo = vec3(1.0, 0.0, 1.0);
        roughness = 0.8;
        metallic = 0.0;
    }
    
    // Write to G-Buffer (matching actual layout)
    float materialFlag = (fragBlockID == WATER_BLOCK_ID) ? 1.0 : 0.0;
    outAlbedo = vec4(albedo, ao);
    outNormal = vec4(normal * 0.5 + 0.5, roughness);  // Encode [-1,1] to [0,1]
    outPosition = vec4(fragWorldPos, 0.0);
    outMetadata = vec4(metallic, materialFlag, 0.0, 0.0);
}
