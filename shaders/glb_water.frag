#version 460 core

in vec2 vUV;
in vec3 vNormal;
in vec3 vWorldPos;

layout(location = 0) out vec3 gAlbedo;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec3 gPosition;
layout(location = 3) out vec4 gMetadata;

void main() {
    // Water - base blue color (will be enhanced in lighting pass)
    gAlbedo = vec3(0.05, 0.2, 0.4);  // Deep ocean blue
    gNormal = normalize(vNormal);
    gPosition = vWorldPos;
    gMetadata = vec4(1.0, 0.0, 0.0, 0.0);  // x=1.0 marks as water for special lighting
}
