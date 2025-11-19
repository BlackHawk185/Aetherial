#version 450

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in flat uint vBlockType;
layout(location = 3) in vec3 vWorldPos;

layout(set = 0, binding = 0) uniform sampler2DArray uBlockTextures;

layout(location = 0) out vec4 outColor;

void main() {
    // Debug: Highlight blocks with invalid texture IDs
    if (vBlockType >= 44u) {
        outColor = vec4(1.0, 0.0, 1.0, 1.0);  // Magenta for invalid block types
        return;
    }
    
    // Sample texture from array
    vec3 texColor = texture(uBlockTextures, vec3(vTexCoord, float(vBlockType))).rgb;
    
    // Simple lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diffuse = max(dot(normalize(vNormal), lightDir), 0.3);
    
    outColor = vec4(texColor * diffuse, 1.0);
}
