#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec3 fragViewDir;
layout(location = 4) in flat uint fragBlockID;

layout(set = 1, binding = 0) uniform sampler2D depthTexture;
layout(set = 1, binding = 1) uniform sampler2D hdrTexture;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec3 cameraPos;
    float time;
} pc;

layout(location = 0) out vec4 outColor;

const uint WATER_BLOCK_ID = 45;
const int MAX_STEPS = 64;
const float STEP_SIZE = 0.5;

vec3 computeSSPR(vec2 screenUV, vec3 worldPos, vec3 normal) {
    vec3 viewDir = normalize(worldPos - pc.cameraPos);
    vec3 reflectDir = reflect(viewDir, normal);
    
    vec3 rayOrigin = worldPos + normal * 0.01; // Offset to avoid self-intersection
    vec3 rayDir = reflectDir;
    
    for (int i = 1; i < MAX_STEPS; i++) {
        vec3 testPos = rayOrigin + rayDir * STEP_SIZE * float(i);
        vec4 clipPos = pc.viewProjection * vec4(testPos, 1.0);
        
        if (clipPos.w <= 0.0) break; // Behind camera
        
        vec3 ndcPos = clipPos.xyz / clipPos.w;
        vec2 testUV = ndcPos.xy * 0.5 + 0.5;
        
        if (testUV.x < 0.0 || testUV.x > 1.0 || testUV.y < 0.0 || testUV.y > 1.0) break;
        
        float sceneDepth = texture(depthTexture, testUV).r;
        
        // In Vulkan depth is 0.0 (near) to 1.0 (far)
        // Ray hits if it passes behind surface (rayDepth > sceneDepth)
        if (ndcPos.z > sceneDepth) {
            float diff = ndcPos.z - sceneDepth;
            if (diff < 0.05) { // Reasonable thickness check
                return texture(hdrTexture, testUV).rgb;
            }
        }
    }
    
    return vec3(0.5, 0.7, 1.0); // Sky fallback
}

void main() {
    if (fragBlockID != WATER_BLOCK_ID) {
        discard;
    }
    
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(pc.cameraPos - fragWorldPos);
    
    vec2 screenUV = gl_FragCoord.xy / textureSize(depthTexture, 0);
    vec3 reflection = computeSSPR(screenUV, fragWorldPos, N);
    
    vec3 waterColor = vec3(0.1, 0.3, 0.5);
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 5.0);
    
    vec3 finalColor = mix(waterColor, reflection, 0.6 + 0.4 * fresnel);
    
    outColor = vec4(finalColor, 0.1);
}
