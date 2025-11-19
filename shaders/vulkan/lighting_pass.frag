#version 450

// Inputs
layout(location = 0) in vec2 vTexCoord;

// G-buffer textures (Set 0)
layout(set = 0, binding = 0) uniform sampler2D gAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gNormal;
layout(set = 0, binding = 2) uniform sampler2D gPosition;
layout(set = 0, binding = 3) uniform sampler2D gMetadata;
layout(set = 0, binding = 4) uniform sampler2D gDepth;

// Shadow/lighting textures (Set 1)
layout(set = 1, binding = 0) uniform sampler2DArrayShadow uLightMap;  // 4 cascades
layout(set = 1, binding = 1) uniform sampler3D uCloudNoiseTex;

// Cascade uniforms (Set 1)
layout(set = 1, binding = 2) uniform CascadeData {
    mat4 cascadeVP[4];
    vec4 cascadeOrthoSizes;  // x,y,z,w = sizes for cascades 0,1,2,3
    vec4 lightTexel;         // x = 1/shadowMapSize
} cascade;

// Push constants
layout(push_constant) uniform LightingParams {
    vec4 sunDirection;     // xyz = direction, w = intensity
    vec4 moonDirection;    // xyz = direction, w = intensity
    vec4 sunColor;         // rgb = color
    vec4 moonColor;        // rgb = color
    vec4 cameraPos;        // xyz = position, w = timeOfDay
    vec4 cascadeParams;    // x = ditherStrength, y = enableCloudShadows
} lighting;

// Output
layout(location = 0) out vec4 fragColor;

// Constants
const float CLOUD_BASE_MIN = 400.0;
const float CLOUD_BASE_MAX = 600.0;
const float CLOUD_COVERAGE = 0.5;
const float CLOUD_DENSITY = 2.0;
const float CLOUD_SPEED = 0.5;
const float CLOUD_SCALE = 0.00015;

// 8-tap Poisson disk for PCF soft lighting
const vec2 POISSON[8] = vec2[8](
    vec2(-0.613392, 0.617481),
    vec2(0.170019, -0.040254),
    vec2(-0.299417, 0.791925),
    vec2(0.645680, 0.493210),
    vec2(-0.651784, 0.717887),
    vec2(0.421003, 0.027070),
    vec2(-0.817194, -0.271096),
    vec2(-0.705374, -0.668203)
);

// Sample cloud shadow occlusion
float sampleCloudShadow(vec3 worldPos, float time) {
    if (lighting.cascadeParams.y < 0.5) return 0.0;
    
    const int SHADOW_SAMPLES = 6;
    float totalOcclusion = 0.0;
    
    vec3 rayDir = -lighting.sunDirection.xyz;
    float stepSize = 40.0;
    
    for (int i = 0; i < SHADOW_SAMPLES; i++) {
        vec3 samplePos = worldPos + rayDir * (float(i) * stepSize);
        
        if (samplePos.y >= CLOUD_BASE_MIN && samplePos.y <= CLOUD_BASE_MAX) {
            vec3 windOffset = vec3(time * CLOUD_SPEED * 0.05, 0.0, time * CLOUD_SPEED * 0.03);
            vec3 noisePos = (samplePos + windOffset) * CLOUD_SCALE;
            
            float noise = texture(uCloudNoiseTex, noisePos).r;
            float density = max(0.0, noise - (1.0 - CLOUD_COVERAGE)) * CLOUD_DENSITY;
            
            float heightInVolume = samplePos.y - CLOUD_BASE_MIN;
            float volumeHeight = CLOUD_BASE_MAX - CLOUD_BASE_MIN;
            float heightGradient = smoothstep(0.0, 30.0, heightInVolume) * 
                                  smoothstep(volumeHeight, volumeHeight - 30.0, heightInVolume);
            
            totalOcclusion += density * heightGradient * 0.2;
        }
    }
    
    return clamp(totalOcclusion, 0.0, 0.85);
}

// Sample single cascade with PCF
float sampleCascade(int cascadeIndex, vec3 worldPos, float bias) {
    vec4 lightSpacePos = cascade.cascadeVP[cascadeIndex] * vec4(worldPos, 1.0);
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    proj = proj * 0.5 + 0.5;
    
    // Out of bounds
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z > 1.0)
        return -1.0;
    
    float current = proj.z - bias;
    
    // PCF radius scaled by cascade ortho size
    int baseCascade = (cascadeIndex / 2) * 2;
    float radiusScale = (cascadeIndex % 2 == 0) ? 1.0 : 
        (cascade.cascadeOrthoSizes[baseCascade] / cascade.cascadeOrthoSizes[baseCascade + 1]);
    float radius = 512.0 * radiusScale * cascade.lightTexel.x;
    
    float lightValue = 0.0;
    for (int i = 0; i < 8; ++i) {
        vec2 offset = POISSON[i] * radius;
        lightValue += texture(uLightMap, vec4(proj.xy + offset, cascadeIndex, current));
    }
    return lightValue / 8.0;
}

// Sample sun light (cascades 0 and 1)
float sampleSunLight(vec3 worldPos, float bias, vec3 normal, vec3 lightDir) {
    float lightNear = sampleCascade(0, worldPos, bias);
    float lightFar = sampleCascade(1, worldPos, bias);
    
    bool nearValid = (lightNear >= 0.0);
    bool farValid = (lightFar >= 0.0);
    
    if (nearValid && farValid) {
        // Blend cascades in transition zone
        vec4 lightSpacePos = cascade.cascadeVP[0] * vec4(worldPos, 1.0);
        vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
        proj = proj * 0.5 + 0.5;
        
        vec2 distToEdge = abs(proj.xy - 0.5) * 2.0;
        float maxDist = max(distToEdge.x, distToEdge.y);
        
        float blendStart = 0.80;
        float blendFactor = smoothstep(blendStart, 1.0, maxDist);
        
        return mix(lightNear, lightFar, blendFactor);
    } else if (nearValid) {
        return lightNear;
    } else if (farValid) {
        return lightFar;
    } else {
        // Ultra far - no shadow map coverage = dark
        return 0.0;
    }
}

// Sample moon light (cascades 2 and 3)
float sampleMoonLight(vec3 worldPos, float bias, vec3 normal, vec3 lightDir) {
    float lightNear = sampleCascade(2, worldPos, bias);
    float lightFar = sampleCascade(3, worldPos, bias);
    
    bool nearValid = (lightNear >= 0.0);
    bool farValid = (lightFar >= 0.0);
    
    if (nearValid && farValid) {
        vec4 lightSpacePos = cascade.cascadeVP[2] * vec4(worldPos, 1.0);
        vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
        proj = proj * 0.5 + 0.5;
        
        vec2 distToEdge = abs(proj.xy - 0.5) * 2.0;
        float maxDist = max(distToEdge.x, distToEdge.y);
        
        float blendStart = 0.80;
        float blendFactor = smoothstep(blendStart, 1.0, maxDist);
        
        return mix(lightNear, lightFar, blendFactor);
    } else if (nearValid) {
        return lightNear;
    } else if (farValid) {
        return lightFar;
    } else {
        // Ultra far - no shadow map coverage = dark
        return 0.0;
    }
}

void main() {
    // Read G-buffer
    vec3 albedo = texture(gAlbedo, vTexCoord).rgb;
    vec3 normal = texture(gNormal, vTexCoord).rgb;
    vec3 worldPos = texture(gPosition, vTexCoord).rgb;
    vec4 metadata = texture(gMetadata, vTexCoord);
    float depth = texture(gDepth, vTexCoord).r;
    
    // Skip sky (no geometry)
    if (depth >= 0.9999) {
        discard;
    }
    
    vec3 N = normalize(normal);
    float materialID = metadata.x;
    uint blockType = uint(materialID * 255.0 + 0.5);
    bool isWater = (blockType == 45u);
    
    // Sample sun light - shadow map only, no ambient
    vec3 L_sun = normalize(-lighting.sunDirection.xyz);
    float bias_sun = 0.0005;
    float sunLightFactor = sampleSunLight(worldPos, bias_sun, N, lighting.sunDirection.xyz);
    
    // Apply cloud shadows
    float cloudShadow = sampleCloudShadow(worldPos, lighting.cameraPos.w);
    sunLightFactor *= (1.0 - cloudShadow);
    
    // Sample moon light - shadow map only, no ambient
    vec3 L_moon = normalize(-lighting.moonDirection.xyz);
    float bias_moon = 0.0005;
    float moonLightFactor = sampleMoonLight(worldPos, bias_moon, N, lighting.moonDirection.xyz);
    
    vec3 finalColor;
    
    if (isWater) {
        // Water rendering with reflections
        vec3 V = normalize(lighting.cameraPos.xyz - worldPos);
        
        float fresnel = 0.9;
        
        vec3 R = reflect(-V, N);
        float skyGradient = R.y * 0.5 + 0.5;
        vec3 skyColor = mix(vec3(0.4, 0.7, 1.0), vec3(0.1, 0.3, 0.6), skyGradient);
        
        // Sun specular
        vec3 H_sun = normalize(L_sun + V);
        float specSun = pow(max(dot(N, H_sun), 0.0), 128.0) * sunLightFactor;
        vec3 sunSpecular = lighting.sunColor.rgb * specSun * lighting.sunDirection.w * 2.0;
        
        // Moon specular
        vec3 H_moon = normalize(L_moon + V);
        float specMoon = pow(max(dot(N, H_moon), 0.0), 64.0) * moonLightFactor;
        vec3 moonSpecular = lighting.moonColor.rgb * specMoon * lighting.moonDirection.w * 0.3;
        
        vec3 waterDiffuse = albedo * (sunLightFactor * lighting.sunDirection.w + 
                                      moonLightFactor * lighting.moonDirection.w * 0.15);
        
        vec3 reflectionColor = skyColor * fresnel;
        finalColor = mix(waterDiffuse, reflectionColor, 0.7) + sunSpecular + moonSpecular;
    } else {
        // Standard material - dark by default, lit where light hits
        vec3 sunContribution = albedo * sunLightFactor * lighting.sunDirection.w;
        vec3 moonContribution = albedo * moonLightFactor * lighting.moonDirection.w * 0.15;
        finalColor = sunContribution + moonContribution;
    }
    
    fragColor = vec4(finalColor, 1.0);
}
