#version 460 core

in vec2 vUV;
in vec3 vNormal;
in vec3 vWorldPos;

layout(location = 0) out vec3 gAlbedo;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec3 gPosition;
layout(location = 3) out vec4 gMetadata;

uniform sampler2D uAlbedoTexture;

void main() {
    // Textured foliage with alpha cutout
    vec4 albedo = texture(uAlbedoTexture, vUV);
    if (albedo.a < 0.3) discard;  // Alpha cutout for grass blades
    
    gAlbedo = albedo.rgb;
    gNormal = normalize(vNormal);
    gPosition = vWorldPos;
    gMetadata = vec4(0.0, 0.0, 0.0, 1.0);  // Standard material
}
