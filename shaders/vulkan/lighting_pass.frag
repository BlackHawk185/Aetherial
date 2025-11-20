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

// 32-tap Poisson disk for high-quality PCF soft shadows
const vec2 POISSON[32] = vec2[32](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790),
    vec2(-0.53023345, -0.60363561), vec2(0.69374341, -0.16296099),
    vec2(-0.15957603, 0.13178897), vec2(-0.52976096, 0.56885791),
    vec2(0.02906645, -0.61384201), vec2(0.25586777, 0.57080173),
    vec2(-0.67520785, -0.15005630), vec2(0.58265102, 0.81282514),
    vec2(-0.37054151, 0.64097851), vec2(0.82294636, -0.56667840),
    vec2(-0.98003292, 0.00492644), vec2(0.35893059, -0.35269195),
    vec2(-0.08734025, 0.47409865), vec2(0.94150668, 0.28232986),
    vec2(-0.42024112, -0.88588715), vec2(0.02940664, 0.94268930)
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

// Sample single cascade with high-quality 32-tap PCF
float sampleCascade(int cascadeIndex, vec3 worldPos, float bias, vec3 normal, vec3 lightDir) {
    vec4 lightSpacePos = cascade.cascadeVP[cascadeIndex] * vec4(worldPos, 1.0);
    vec3 ndc = lightSpacePos.xyz / lightSpacePos.w;
    
    // Out of bounds (Vulkan NDC: xy = [-1,1], z = [0,1])
    if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0 || ndc.z > 1.0)
        return -1.0;
    
    // Convert XY from NDC [-1, 1] to texture coordinates [0, 1]. Z is already [0, 1] in Vulkan.
    vec3 shadowCoord = vec3(ndc.xy * 0.5 + 0.5, ndc.z);
    
    // Slope-scale depth bias (reduces peter-panning and acne)
    float slopeBias = max(0.002 * (1.0 - dot(normal, lightDir)), bias);
    float current = shadowCoord.z - slopeBias;
    
    // Early exit optimization: sample center first with same depth as PCF
    float centerShadow = texture(uLightMap, vec4(shadowCoord.xy, cascadeIndex, current));
    if (centerShadow > 0.99) return 1.0;  // Fully lit, skip PCF
    
    // PCF radius scaled by cascade ortho size
    int baseCascade = (cascadeIndex / 2) * 2;
    float radiusScale = (cascadeIndex % 2 == 0) ? 1.0 : 
        (cascade.cascadeOrthoSizes[baseCascade] / cascade.cascadeOrthoSizes[baseCascade + 1]);
    float radius = 512.0 * radiusScale * cascade.lightTexel.x;
    
    // 32-tap Poisson disk PCF for high-quality soft shadows
    float lightValue = 0.0;
    for (int i = 0; i < 32; ++i) {
        vec2 offset = POISSON[i] * radius;
        lightValue += texture(uLightMap, vec4(shadowCoord.xy + offset, cascadeIndex, current));
    }
    return lightValue / 32.0;
}

// Sample sun light (cascades 0 and 1)
float sampleSunLight(vec3 worldPos, float bias, vec3 normal, vec3 lightDir) {
    float lightNear = sampleCascade(0, worldPos, bias, normal, lightDir);
    float lightFar = sampleCascade(1, worldPos, bias, normal, lightDir);
    
    bool nearValid = (lightNear >= 0.0);
    bool farValid = (lightFar >= 0.0);
    
    if (nearValid && farValid) {
        // Blend cascades in transition zone
        vec4 lightSpacePos = cascade.cascadeVP[0] * vec4(worldPos, 1.0);
        vec3 ndc = lightSpacePos.xyz / lightSpacePos.w;
        
        vec2 distToEdge = abs(ndc.xy);
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
    float lightNear = sampleCascade(2, worldPos, bias, normal, lightDir);
    float lightFar = sampleCascade(3, worldPos, bias, normal, lightDir);
    
    bool nearValid = (lightNear >= 0.0);
    bool farValid = (lightFar >= 0.0);
    
    if (nearValid && farValid) {
        vec4 lightSpacePos = cascade.cascadeVP[2] * vec4(worldPos, 1.0);
        vec3 ndc = lightSpacePos.xyz / lightSpacePos.w;
        
        vec2 distToEdge = abs(ndc.xy);
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
