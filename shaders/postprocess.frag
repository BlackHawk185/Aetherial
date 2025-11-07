#version 460 core

// Input from vertex shader
in vec2 v_TexCoord;

// HDR input
uniform sampler2D u_HDRTexture;

// Post-processing parameters
uniform float u_Exposure;
uniform float u_Gamma;

// Output
out vec4 FragColor;

// Tone mapping functions
vec3 reinhardToneMapping(vec3 color, float exposure) {
    color *= exposure;
    return color / (1.0 + color);
}

vec3 acesToneMapping(vec3 color, float exposure) {
    color *= exposure;
    
    // ACES tone mapping curve fit
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 gammaCorrection(vec3 color, float gamma) {
    return pow(color, vec3(1.0 / gamma));
}

void main() {
    // Sample HDR color
    vec3 hdrColor = texture(u_HDRTexture, v_TexCoord).rgb;
    
    // Tone mapping
    vec3 ldrColor = acesToneMapping(hdrColor, u_Exposure);
    
    // Gamma correction
    ldrColor = gammaCorrection(ldrColor, u_Gamma);
    
    // TODO: Add additional post-processing effects:
    // - Bloom
    // - Chromatic aberration
    // - Film grain
    // - Color grading
    // - FXAA
    
    FragColor = vec4(ldrColor, 1.0);
}
