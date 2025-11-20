#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;

// G-Buffer outputs (match VulkanGBuffer.cpp format)
layout(location = 0) out vec4 outAlbedo;      // RGB: albedo, A: unused
layout(location = 1) out vec4 outNormal;      // RGB: world normal, A: unused
layout(location = 2) out vec4 outPBR;         // R: roughness, G: metallic, B: ao, A: unused
layout(location = 3) out vec4 outEmissive;    // RGB: emissive color, A: unused

void main() {
    // Magenta color for missing textures/fallback
    vec3 albedo = vec3(1.0, 0.0, 1.0);
    
    // Normalize interpolated normal
    vec3 normal = normalize(fragNormal);
    
    // PBR properties for magenta debug material
    float roughness = 0.8;
    float metallic = 0.0;
    float ao = 1.0;
    
    // Write to G-Buffer
    outAlbedo = vec4(albedo, 1.0);
    outNormal = vec4(normal * 0.5 + 0.5, 1.0);  // Encode [-1,1] to [0,1]
    outPBR = vec4(roughness, metallic, ao, 1.0);
    outEmissive = vec4(0.0, 0.0, 0.0, 1.0);
}
