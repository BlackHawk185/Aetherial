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

void main() {
    vec3 albedo;
    float roughness;
    float metallic;
    float ao = 1.0;
    
    // Normalize interpolated normal
    vec3 normal = normalize(fragNormal);
    
    // Check if this is a water block
    if (fragBlockID == WATER_BLOCK_ID) {
        // Water material properties - flat surface for testing reflections
        albedo = vec3(0.05, 0.15, 0.25);  // Dark blue-green water
        roughness = 0.08;  // Very smooth for reflections
        metallic = 0.15;   // Low metallic - water is dielectric
        ao = 1.0;          // Full AO
        
        // Use flat normal for testing (no wave perturbation)
        normal = vec3(0.0, 1.0, 0.0);
        
        // Water gets special treatment in lighting pass via metadata
        // The lighting shader detects water and applies SSR
    } else {
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
