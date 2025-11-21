#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;
layout(set = 0, binding = 1) uniform sampler2D ssrBuffer;
layout(set = 0, binding = 2) uniform sampler2D metadataBuffer;
layout(set = 0, binding = 3) uniform sampler2D normalBuffer;
layout(set = 0, binding = 4) uniform sampler2D positionBuffer;

layout(push_constant) uniform PushConstants {
    vec3 cameraPos;
    float normalFOV;    // 70 degrees
    float wideFOV;      // 120 degrees
} pc;

void main() {
    // Map viewport UVs (70° FOV) to wide-rendered buffer UVs (120° FOV)
    // Center crop: scale and offset to sample from center of wide render
    float fovRatio = pc.normalFOV / pc.wideFOV;  // ~0.583
    vec2 centeredUV = (fragTexCoord - 0.5) * fovRatio + 0.5;
    
    vec4 metadata = texture(metadataBuffer, centeredUV);
    
    // Discard sky pixels (no geometry)
    if (metadata.r == 0.0 && metadata.g == 0.0) discard;
    
    vec3 hdrColor = texture(hdrBuffer, centeredUV).rgb;
    vec4 ssrData = texture(ssrBuffer, centeredUV);
    
    // Water detection
    bool isWaterOBJ = metadata.g > 0.9;
    bool isWaterVoxel = (metadata.r > 0.17 && metadata.r < 0.18);
    bool isWater = isWaterOBJ || isWaterVoxel;
    
    vec3 finalColor = hdrColor;
    
    // Apply SSR to water surfaces
    if (isWater && ssrData.a > 0.0) {
        // Get view direction for Fresnel
        vec3 worldPos = texture(positionBuffer, centeredUV).rgb;
        vec3 worldNormal = texture(normalBuffer, centeredUV).rgb * 2.0 - 1.0;
        vec3 V = normalize(pc.cameraPos - worldPos);
        
        // Fresnel - more reflection at grazing angles
        float fresnel = pow(1.0 - max(dot(V, worldNormal), 0.0), 2.0);
        fresnel = mix(0.5, 1.0, fresnel);  // Much stronger base reflection
        
        // Fallback sky gradient for missed rays
        vec3 R = reflect(-V, worldNormal);
        float skyGradient = R.y * 0.5 + 0.5;
        vec3 skyColor = mix(vec3(0.5, 0.8, 1.0), vec3(0.2, 0.4, 0.7), skyGradient);
        
        // Blend SSR with sky fallback based on hit strength
        vec3 reflectionColor = mix(skyColor, ssrData.rgb, ssrData.a);
        
        // Strong reflections - water is very reflective
        finalColor = mix(hdrColor, reflectionColor, fresnel * 0.95);
    }
    
    outColor = vec4(finalColor, 1.0);
}
