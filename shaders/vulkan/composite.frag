#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;
layout(set = 0, binding = 1) uniform sampler2D ssrBuffer;
layout(set = 0, binding = 2) uniform sampler2D metadataBuffer;
layout(set = 0, binding = 3) uniform sampler2D normalBuffer;
layout(set = 0, binding = 4) uniform sampler2D positionBuffer;
layout(set = 0, binding = 5) uniform sampler2D depthBuffer;

layout(push_constant) uniform PushConstants {
    vec3 cameraPos;
} pc;

void main() {
    // Check depth to detect sky (depth = 1.0 means no geometry)
    float depth = texture(depthBuffer, fragTexCoord).r;
    if (depth >= 0.9999) discard;
    
    vec3 hdrColor = texture(hdrBuffer, fragTexCoord).rgb;
    vec4 metadata = texture(metadataBuffer, fragTexCoord);
    vec4 ssrData = texture(ssrBuffer, fragTexCoord);
    
    // Get material properties
    float metallic = metadata.r;
    vec4 normalRoughness = texture(normalBuffer, fragTexCoord);
    float roughness = normalRoughness.a;
    
    vec3 finalColor = hdrColor;
    
    // Apply SSR if we have reflection data
    if (ssrData.a > 0.01) {
        vec3 worldPos = texture(positionBuffer, fragTexCoord).rgb;
        vec3 worldNormal = normalRoughness.rgb * 2.0 - 1.0;
        vec3 V = normalize(pc.cameraPos - worldPos);
        
        // Calculate Fresnel for all reflective surfaces
        vec3 F0 = mix(vec3(0.04), hdrColor, metallic);
        float cosTheta = max(dot(worldNormal, V), 0.0);
        float fresnel = pow(1.0 - cosTheta, 5.0);
        float F = mix(F0.r, 1.0, fresnel);
        
        // Fallback sky gradient for incomplete reflections
        vec3 R = reflect(-V, worldNormal);
        float skyGradient = R.y * 0.5 + 0.5;
        vec3 skyColor = mix(vec3(0.5, 0.8, 1.0), vec3(0.2, 0.4, 0.7), skyGradient);
        
        // Mix SSR with sky fallback based on hit confidence
        vec3 reflectionColor = mix(skyColor, ssrData.rgb, ssrData.a);
        
        // Blend reflection based on Fresnel and roughness
        float reflectionStrength = F * (1.0 - roughness);
        finalColor = mix(hdrColor, reflectionColor, reflectionStrength);
    }
    
    outColor = vec4(finalColor, 1.0);
}
