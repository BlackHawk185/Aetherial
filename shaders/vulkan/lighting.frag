#version 450

// Inputs
layout(location = 0) in vec2 vTexCoord;

// G-buffer textures
layout(set = 0, binding = 0) uniform sampler2D gAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gNormal;
layout(set = 0, binding = 2) uniform sampler2D gPosition;
layout(set = 0, binding = 3) uniform sampler2D gMetadata;
layout(set = 0, binding = 4) uniform sampler2D gDepth;

// Lighting uniforms
layout(push_constant) uniform LightingParams {
    vec4 sunDirection;    // xyz = direction, w = intensity
    vec4 moonDirection;   // xyz = direction, w = intensity
    vec4 sunColor;        // rgb = color
    vec4 moonColor;       // rgb = color
    vec4 cameraPos;       // xyz = position
} lighting;

// Output
layout(location = 0) out vec4 fragColor;

void main() {
    // Read G-buffer
    vec3 albedo = texture(gAlbedo, vTexCoord).rgb;
    vec3 normal = texture(gNormal, vTexCoord).rgb;
    vec3 worldPos = texture(gPosition, vTexCoord).rgb;
    vec2 metadata = texture(gMetadata, vTexCoord).rg;
    float depth = texture(gDepth, vTexCoord).r;
    
    // Sky background (no geometry)
    if (depth >= 1.0) {
        fragColor = vec4(0.5, 0.7, 1.0, 1.0); // Sky blue
        return;
    }
    
    // Sun lighting
    float sunNdotL = max(dot(normal, lighting.sunDirection.xyz), 0.0);
    vec3 sunDiffuse = lighting.sunColor.rgb * sunNdotL * lighting.sunDirection.w;
    
    // Moon lighting
    float moonNdotL = max(dot(normal, lighting.moonDirection.xyz), 0.0);
    vec3 moonDiffuse = lighting.moonColor.rgb * moonNdotL * lighting.moonDirection.w;
    
    // Combine lighting (dark by default)
    vec3 lighting = sunDiffuse + moonDiffuse;
    vec3 finalColor = albedo * lighting;
    
    fragColor = vec4(finalColor, 1.0);
}
