#version 450

// PBR functions
const float PI = 3.14159265359;

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / denom;
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float denom = NdotV * (1.0 - k) + k;
    return NdotV / denom;
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec3 calculatePBR(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness, vec3 radiance) {
    vec3 H = normalize(V + L);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
    vec3 specular = numerator / denominator;
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = kD * albedo / PI;
    return (diffuse + specular) * radiance * NdotL;
}

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

// SSR texture (Set 1)
layout(set = 1, binding = 3) uniform sampler2D uSSRReflections;

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
// Prefers cascade with actual shadow data (blocker-aware selection)
float sampleSunLight(vec3 worldPos, float bias, vec3 normal, vec3 lightDir) {
    float lightNear = sampleCascade(0, worldPos, bias, normal, lightDir);
    float lightFar = sampleCascade(1, worldPos, bias, normal, lightDir);
    
    bool nearValid = (lightNear >= 0.0);
    bool farValid = (lightFar >= 0.0);
    
    if (nearValid && farValid) {
        // Both cascades have data - check which has actual shadow (blocker present)
        // If near cascade is fully lit (1.0), it means no blocker in near range
        // Use far cascade if it has shadow data from distant blockers
        const float FULLY_LIT_THRESHOLD = 0.95;
        
        bool nearHasBlocker = (lightNear < FULLY_LIT_THRESHOLD);
        bool farHasBlocker = (lightFar < FULLY_LIT_THRESHOLD);
        
        if (nearHasBlocker && farHasBlocker) {
            // Both have blockers - blend in transition zone
            vec4 lightSpacePos = cascade.cascadeVP[0] * vec4(worldPos, 1.0);
            vec3 ndc = lightSpacePos.xyz / lightSpacePos.w;
            vec2 distToEdge = abs(ndc.xy);
            float maxDist = max(distToEdge.x, distToEdge.y);
            float blendStart = 0.80;
            float blendFactor = smoothstep(blendStart, 1.0, maxDist);
            return mix(lightNear, lightFar, blendFactor);
        } else if (nearHasBlocker) {
            return lightNear;  // Near has shadow, use it
        } else if (farHasBlocker) {
            return lightFar;   // Only far has shadow (distant mountain)
        } else {
            // Both fully lit - use near for consistency
            return lightNear;
        }
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
// Prefers cascade with actual shadow data (blocker-aware selection)
float sampleMoonLight(vec3 worldPos, float bias, vec3 normal, vec3 lightDir) {
    float lightNear = sampleCascade(2, worldPos, bias, normal, lightDir);
    float lightFar = sampleCascade(3, worldPos, bias, normal, lightDir);
    
    bool nearValid = (lightNear >= 0.0);
    bool farValid = (lightFar >= 0.0);
    
    if (nearValid && farValid) {
        // Both cascades have data - check which has actual shadow (blocker present)
        const float FULLY_LIT_THRESHOLD = 0.95;
        
        bool nearHasBlocker = (lightNear < FULLY_LIT_THRESHOLD);
        bool farHasBlocker = (lightFar < FULLY_LIT_THRESHOLD);
        
        if (nearHasBlocker && farHasBlocker) {
            // Both have blockers - blend in transition zone
            vec4 lightSpacePos = cascade.cascadeVP[2] * vec4(worldPos, 1.0);
            vec3 ndc = lightSpacePos.xyz / lightSpacePos.w;
            vec2 distToEdge = abs(ndc.xy);
            float maxDist = max(distToEdge.x, distToEdge.y);
            float blendStart = 0.80;
            float blendFactor = smoothstep(blendStart, 1.0, maxDist);
            return mix(lightNear, lightFar, blendFactor);
        } else if (nearHasBlocker) {
            return lightNear;  // Near has shadow, use it
        } else if (farHasBlocker) {
            return lightFar;   // Only far has shadow (distant mountain)
        } else {
            // Both fully lit - use near for consistency
            return lightNear;
        }
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
    vec4 albedoAO = texture(gAlbedo, vTexCoord);
    vec3 albedo = albedoAO.rgb;
    float ao = albedoAO.a;
    vec4 normalRoughness = texture(gNormal, vTexCoord);
    vec3 normal = normalRoughness.rgb;
    float roughness = normalRoughness.a;
    vec3 worldPos = texture(gPosition, vTexCoord).rgb;
    vec4 metadata = texture(gMetadata, vTexCoord);
    float depth = texture(gDepth, vTexCoord).r;
    
    if (depth >= 0.9999) {
        discard;
    }
    
    vec3 N = normalize(normal);
    vec3 V = normalize(lighting.cameraPos.xyz - worldPos);
    float materialID = metadata.x;
    uint blockType = uint(materialID * 255.0 + 0.5);
    bool isWater = (blockType == 45u);
    
    vec3 L_sun = normalize(-lighting.sunDirection.xyz);
    float bias_sun = 0.0005;
    float sunLightFactor = sampleSunLight(worldPos, bias_sun, N, lighting.sunDirection.xyz);
    float cloudShadow = sampleCloudShadow(worldPos, lighting.cameraPos.w);
    sunLightFactor *= (1.0 - cloudShadow);
    
    vec3 L_moon = normalize(-lighting.moonDirection.xyz);
    float bias_moon = 0.0005;
    float moonLightFactor = sampleMoonLight(worldPos, bias_moon, N, lighting.moonDirection.xyz);
    
    vec4 ssrSample = texture(uSSRReflections, vTexCoord);
    vec3 ssrColor = ssrSample.rgb;
    float ssrStrength = ssrSample.a;
    
    vec3 finalColor;
    
    if (isWater) {
        float waterMetallic = 0.02;
        float waterRoughness = 0.1;
        vec3 sunRadiance = lighting.sunColor.rgb * lighting.sunDirection.w * sunLightFactor;
        vec3 moonRadiance = lighting.moonColor.rgb * lighting.moonDirection.w * 0.15 * moonLightFactor;
        vec3 sunPBR = calculatePBR(N, V, L_sun, albedo, waterMetallic, waterRoughness, sunRadiance);
        vec3 moonPBR = calculatePBR(N, V, L_moon, albedo, waterMetallic, waterRoughness, moonRadiance);
        vec3 F0 = mix(vec3(0.04), albedo, waterMetallic);
        vec3 F = fresnelSchlick(max(dot(N, V), 0.0), F0);
        vec3 reflectionColor = mix(vec3(0.0), ssrColor, ssrStrength);
        finalColor = sunPBR + moonPBR + reflectionColor * F;
    } else {
        float metallic = 0.0;
        float effectiveRoughness = roughness;
        vec3 sunRadiance = lighting.sunColor.rgb * lighting.sunDirection.w * sunLightFactor;
        vec3 moonRadiance = lighting.moonColor.rgb * lighting.moonDirection.w * 0.15 * moonLightFactor;
        vec3 sunPBR = calculatePBR(N, V, L_sun, albedo, metallic, effectiveRoughness, sunRadiance);
        vec3 moonPBR = calculatePBR(N, V, L_moon, albedo, metallic, effectiveRoughness, moonRadiance);
        vec3 reflectionTerm = ssrColor * ssrStrength * (1.0 - effectiveRoughness) * metallic;
        finalColor = (sunPBR + moonPBR) * ao + reflectionTerm;
    }
    
    fragColor = vec4(finalColor, 1.0);
}
