#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aTexCoord;

// Per-instance data (pre-transformed world space)
layout(location = 2) in vec3 iWorldPosition;  // Pre-transformed world-space center position
layout(location = 3) in float iWidth;
layout(location = 4) in float iHeight;
layout(location = 5) in uint iPackedNormal;   // Packed 10:10:10:2 normal
layout(location = 6) in uint iBlockType;
layout(location = 7) in uint iFaceDir;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
} pc;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out flat uint vBlockType;

// Rotation matrices for each face direction (compile-time constants)
// These rotate the unit quad to face the correct direction
const mat3 FACE_ROTATIONS[6] = mat3[6](
    mat3(1,0,0, 0,0,1, 0,1,0),   // 0: -Y (bottom): XY -> XZ
    mat3(1,0,0, 0,0,-1, 0,1,0),  // 1: +Y (top): XY -> XZ, flip Z
    mat3(1,0,0, 0,1,0, 0,0,1),   // 2: -Z (back): identity
    mat3(-1,0,0, 0,1,0, 0,0,1),  // 3: +Z (front): flip X
    mat3(0,0,1, 0,1,0, 1,0,0),   // 4: -X (left): XY -> ZY
    mat3(0,0,-1, 0,1,0, 1,0,0)   // 5: +X (right): XY -> ZY, flip Z
);

void main() {
    // Scale unit quad by width/height
    vec3 scaledPos = aPosition * vec3(iWidth, iHeight, 1.0);
    
    // Rotate based on face direction (array lookup, no branches)
    vec3 rotatedPos = FACE_ROTATIONS[iFaceDir] * scaledPos;
    
    // Add to pre-transformed world position
    vec3 worldPos = iWorldPosition + rotatedPos;
    
    gl_Position = pc.viewProjection * vec4(worldPos, 1.0);
    
    vTexCoord = aTexCoord;
    
    // Unpack normal from 10:10:10:2 format
    uint nx = (iPackedNormal) & 0x3FFu;
    uint ny = (iPackedNormal >> 10u) & 0x3FFu;
    uint nz = (iPackedNormal >> 20u) & 0x3FFu;
    vNormal = vec3(float(nx) / 1023.0 * 2.0 - 1.0,
                   float(ny) / 1023.0 * 2.0 - 1.0,
                   float(nz) / 1023.0 * 2.0 - 1.0);
    vNormal = normalize(vNormal);
    
    vBlockType = iBlockType;
}
