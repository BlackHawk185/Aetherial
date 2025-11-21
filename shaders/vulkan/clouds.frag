#version 460 core

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uDepthTexture;
layout(set = 0, binding = 1) uniform sampler3D uNoiseTexture;

layout(push_constant) uniform CloudParams {
    mat4 invProjection;
    mat4 invView;
    vec4 cameraPosition;       // xyz = position
    vec4 sunDirection;         // xyz = direction, w = intensity
    vec4 cloudParams;          // x = coverage, y = density, z = speed, w = timeOfDay
} params;

// Cloud appearance parameters
const float CLOUD_SCALE = 0.001;

// Raymarching parameters
const int MAX_STEPS = 32;
const float MAX_DISTANCE = 1000.0;

// Blue noise for dithering (interleaved gradient noise)
float interleavedGradientNoise(vec2 pixelCoord) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(pixelCoord, magic.xy)));
}

// Reconstruct world position from depth
vec3 worldPositionFromDepth(vec2 uv, float depth) {
    vec4 clipSpace = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewSpace = params.invProjection * clipSpace;
    viewSpace /= viewSpace.w;
    vec4 worldSpace = params.invView * viewSpace;
    return worldSpace.xyz;
}

// Sample cloud density from 3D noise
float sampleCloudDensity(vec3 position, float time) {
    // Apply wind offset
    float cloudSpeed = params.cloudParams.z;
    vec3 windOffset = vec3(time * cloudSpeed * 0.05, 0.0, time * cloudSpeed * 0.03);
    vec3 samplePos = (position + windOffset) * CLOUD_SCALE;
    
    // Multi-octave 3D noise sampling
    float noise = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    float maxValue = 0.0;
    
    // Different offsets per octave to eliminate tiling
    vec3 octaveOffsets[4] = vec3[4](
        vec3(0.0, 0.0, 0.0),
        vec3(123.456, 789.012, 345.678),
        vec3(901.234, 567.890, 123.456),
        vec3(456.789, 234.567, 890.123)
    );
    
    for (int i = 0; i < 4; i++) {
        vec3 offsetPos = samplePos * frequency + octaveOffsets[i];
        noise += texture(uNoiseTexture, offsetPos).r * amplitude;
        maxValue += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    
    noise /= maxValue;
    
    // Apply coverage and remap
    float coverage = params.cloudParams.x;
    float densityMult = params.cloudParams.y;
    
    // Smoothstep density for softer transitions
    return smoothstep(0.0, 1.0, max(0.0, noise - (1.0 - coverage))) * densityMult;
}

// Simple light scattering
float lightEnergy(vec3 position, float time) {
    // Sample in sun direction for better scattering
    vec3 sunDir = params.sunDirection.xyz;
    vec3 lightSamplePos = position + sunDir * 30.0;
    float density = sampleCloudDensity(lightSamplePos, time);
    
    // Beer-Lambert law
    return exp(-density * 2.0);
}

void main() {
    // Get scene depth
    float sceneDepth = texture(uDepthTexture, vUV).r;
    vec3 sceneWorldPos = worldPositionFromDepth(vUV, sceneDepth);
    vec3 cameraPos = params.cameraPosition.xyz;
    float sceneDistance = length(sceneWorldPos - cameraPos);
    
    // Reconstruct ray direction
    vec3 rayDir = normalize(sceneWorldPos - cameraPos);
    
    // Raymarch through full distance (composite depth test handles occlusion)
    float tMin = 0.0;
    float tMax = MAX_DISTANCE;
    
    // Raymarch through cloud layer
    float timeOfDay = params.cloudParams.w;
    float stepSize = (tMax - tMin) / float(MAX_STEPS);
    
    // Apply blue noise offset to break up banding
    vec2 pixelCoord = gl_FragCoord.xy;
    float jitter = interleavedGradientNoise(pixelCoord);
    float t = tMin + jitter * stepSize;
    
    float transmittance = 1.0;
    vec3 cloudColor = vec3(0.0);
    
    for (int i = 0; i < MAX_STEPS; i++) {
        if (transmittance < 0.01) break;
        
        vec3 samplePos = cameraPos + rayDir * t;
        float density = sampleCloudDensity(samplePos, timeOfDay);
        
        if (density > 0.001) {
            float light = lightEnergy(samplePos, timeOfDay);
            
            // Sun color - warm white
            float sunIntensity = params.sunDirection.w;
            vec3 sunColor = vec3(1.0, 0.95, 0.85) * sunIntensity;
            
            // Ambient sky color
            vec3 ambientColor = vec3(0.5, 0.6, 0.7) * 0.3;
            
            // Combine lighting
            vec3 lighting = sunColor * light + ambientColor;
            
            // Accumulate color
            float densityStep = density * stepSize;
            cloudColor += transmittance * lighting * densityStep;
            transmittance *= exp(-densityStep);
        }
        
        t += stepSize;
        if (t >= tMax) break;
    }
    
    float alpha = 1.0 - transmittance;
    
    if (alpha < 0.01) discard;
    
    FragColor = vec4(cloudColor, alpha);
}
