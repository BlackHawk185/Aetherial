#version 460 core
layout(location = 0) in vec3 vWorldPos;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 sunDir;
    float sunIntensity;
    vec3 moonDir;
    float moonIntensity;
    vec3 cameraPos;
    float timeOfDay;
    float sunSize;
    float sunGlow;
    float moonSize;
    float exposure;
} pc;

layout(location = 0) out vec4 FragColor;

// Generate pseudo-random value for star positions
float hash(vec3 p) {
    p = fract(p * 0.3183099);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Generate starfield
vec3 generateStars(vec3 rayDir, vec3 skyColor) {
    vec3 p = rayDir * 100.0;
    vec3 gridPos = floor(p);
    vec3 localPos = fract(p);
    
    vec3 stars = vec3(0.0);
    
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            for (int z = -1; z <= 1; z++) {
                vec3 cellPos = gridPos + vec3(x, y, z);
                float h = hash(cellPos);
                
                if (h > 0.999) {
                    vec3 starPos = cellPos + vec3(
                        hash(cellPos + vec3(1.0, 2.0, 3.0)),
                        hash(cellPos + vec3(4.0, 5.0, 6.0)),
                        hash(cellPos + vec3(7.0, 8.0, 9.0))
                    );
                    
                    vec3 starDir = normalize(starPos);
                    float alignment = dot(rayDir, starDir);
                    
                    if (alignment > 0.9999) {
                        float brightness = pow(max(0.0, (alignment - 0.9999) / 0.0001), 3.0);
                        vec3 dayTimeSkyColor = vec3(0.5, 0.7, 1.0);
                        vec3 starColor = dayTimeSkyColor;
                        stars += starColor * brightness * 0.4;
                    }
                }
            }
        }
    }
    
    return stars;
}

// Calculate sky gradient
vec3 calculateSkyGradient(vec3 rayDir, vec3 sunDir) {
    float height = rayDir.y;
    float sunHeight = -sunDir.y;
    
    vec3 daySky = vec3(0.5, 0.7, 1.0);
    vec3 dayHorizon = vec3(0.8, 0.9, 1.0);
    
    vec3 nightSky = vec3(0.01, 0.01, 0.05);
    vec3 nightHorizon = vec3(0.02, 0.02, 0.08);
    
    vec3 sunsetSky = vec3(0.3, 0.2, 0.4);
    vec3 sunsetHorizon = vec3(1.0, 0.5, 0.2);
    
    vec3 skyColor, horizonColor;
    
    if (sunHeight > 0.3) {
        float t = clamp((sunHeight - 0.3) / 0.7, 0.0, 1.0);
        skyColor = mix(sunsetSky, daySky, t);
        horizonColor = mix(sunsetHorizon, dayHorizon, t);
    } else if (sunHeight > -0.3) {
        skyColor = sunsetSky;
        horizonColor = sunsetHorizon;
    } else {
        float t = clamp((-sunHeight - 0.3) / 0.7, 0.0, 1.0);
        skyColor = mix(sunsetSky, nightSky, t);
        horizonColor = mix(sunsetHorizon, nightHorizon, t);
    }
    
    float gradientT = smoothstep(-0.5, 0.8, height);
    return mix(horizonColor, skyColor, gradientT);
}

// Calculate sun disc and glow
vec3 calculateSunDisc(vec3 rayDir, vec3 sunDir, float sunIntensity) {
    float alignment = dot(rayDir, -sunDir);
    float angularDist = acos(clamp(alignment, -1.0, 1.0));
    
    float sunDisc = 1.0 - smoothstep(0.0, pc.sunSize, angularDist);
    float sunGlow = 1.0 - smoothstep(0.0, pc.sunSize * pc.sunGlow, angularDist);
    sunGlow = pow(sunGlow, 2.0);
    
    vec3 sunColor = vec3(1.0, 0.95, 0.8);
    vec3 sun = sunColor * (sunDisc * 50.0 + sunGlow * 2.0) * sunIntensity;
    
    return sun;
}

// Calculate moon disc
vec3 calculateMoonDisc(vec3 rayDir, vec3 moonDir, float moonIntensity) {
    float alignment = dot(rayDir, -moonDir);
    float angularDist = acos(clamp(alignment, -1.0, 1.0));
    
    float moonDisc = 1.0 - smoothstep(0.0, pc.moonSize, angularDist);
    
    vec3 moonColor = vec3(0.9, 0.95, 1.0);
    vec3 moon = moonColor * moonDisc * 8.0 * moonIntensity;
    
    return moon;
}

void main() {
    vec3 rayDir = normalize(vWorldPos);
    
    vec3 skyColor = calculateSkyGradient(rayDir, pc.sunDir);
    vec3 moonContribution = calculateMoonDisc(rayDir, pc.moonDir, pc.moonIntensity);
    vec3 sunContribution = calculateSunDisc(rayDir, pc.sunDir, pc.sunIntensity);
    vec3 starContribution = generateStars(rayDir, skyColor);
    
    vec3 finalColor = skyColor + moonContribution + sunContribution + starContribution;
    finalColor *= pc.exposure;
    
    FragColor = vec4(finalColor, 1.0);
}
