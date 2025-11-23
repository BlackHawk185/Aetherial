#version 450

// Vertex pulling: fetch QuadFace from SSBO using gl_VertexIndex
// Island-relative positions + per-island transforms

// QuadFace struct (32 bytes bit-packed)
// CRITICAL: position is island-relative and NEVER packed (islands move, need precision)
struct QuadFace {
    vec3 position;       // Island-relative CORNER position (12 bytes, offset 0)
    uint packed0;        // width(16) | height(16) - packed dimensions (offset 12)
    uint packed1;        // normal(30) | faceDir(3) - packed normal + face (offset 16) [1 bit unused]
    uint packed2;        // blockType(16) | islandID(16) - packed IDs (offset 20)
    uint _padding0;      // Align to 32 bytes (offset 24)
    uint _padding1;      // Align to 32 bytes (offset 28)
};

// Unit quad vertices (hardcoded, no vertex buffer needed)
// Industry standard: corner-based [0,0] to [1,1]
// Matches original index pattern: {0,1,2, 2,3,0}
// Vertices: 0=BL, 1=BR, 2=TR, 3=TL
const vec3 UNIT_QUAD_POSITIONS[6] = vec3[6](
    vec3(0.0, 0.0, 0.0),  // 0: Bottom-left (corner origin)
    vec3(1.0, 0.0, 0.0),  // 1: Bottom-right
    vec3(1.0, 1.0, 0.0),  // 2: Top-right
    vec3(1.0, 1.0, 0.0),  // 2: Top-right (repeated for triangle 2)
    vec3(0.0, 1.0, 0.0),  // 3: Top-left
    vec3(0.0, 0.0, 0.0)   // 0: Bottom-left (repeated)
);

const vec2 UNIT_QUAD_TEXCOORDS[6] = vec2[6](
    vec2(0.0, 0.0),  // BL
    vec2(1.0, 0.0),  // BR
    vec2(1.0, 1.0),  // TR
    vec2(1.0, 1.0),  // TR (repeated)
    vec2(0.0, 1.0),  // TL
    vec2(0.0, 0.0)   // BL (repeated)
);

// Rotation matrices for each face direction (COLUMN-MAJOR ORDER)
// Industry standard: 0=-X, 1=+X, 2=-Y, 3=+Y, 4=-Z, 5=+Z
// Unit quad [0,0]→[1,1]: right=(1,0), up=(0,1), CCW winding BL→BR→TR
// Matrices map: right→world_right, up→world_up, cross(right,up)→outward_normal
const mat3 FACE_ROTATIONS[6] = mat3[6](
    mat3(0,0,1,  0,1,0,  -1,0,0),   // 0: -X: right=+Z, up=+Y, normal=-X
    mat3(0,0,-1,  0,1,0,  1,0,0),   // 1: +X: right=-Z, up=+Y, normal=+X
    mat3(1,0,0,  0,0,1,  0,-1,0),   // 2: -Y: right=+X, up=+Z, normal=-Y
    mat3(1,0,0,  0,0,-1,  0,1,0),   // 3: +Y: right=+X, up=-Z, normal=+Y
    mat3(-1,0,0,  0,1,0,  0,0,-1),  // 4: -Z: right=-X, up=+Y, normal=-Z
    mat3(1,0,0,  0,1,0,  0,0,1)     // 5: +Z: right=+X, up=+Y, normal=+Z
);

// Instance buffer (QuadFace array)
layout(set = 0, binding = 2, std430) readonly buffer InstanceBuffer {
    QuadFace quads[];
};

// Island transforms as vec4[4] to avoid driver bugs with mat4 in SSBO
layout(set = 0, binding = 1, std430) readonly buffer IslandTransforms {
    vec4 transformData[];  // Each mat4 = 4 consecutive vec4s
};

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    uint baseQuadIndex;  // Offset into SSBO for this draw call
} pc;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out flat uint vBlockType;
layout(location = 3) out vec3 vWorldPos;

void main() {
    uint quadIndex = pc.baseQuadIndex + (gl_VertexIndex / 6u);
    uint vertexIndex = gl_VertexIndex % 6u;
    
    // Fetch quad from SSBO
    QuadFace quad = quads[quadIndex];
    
    // Unpack dimensions from packed0 (width in low 16, height in high 16)
    float width = float(quad.packed0 & 0xFFFFu) / 256.0;
    float height = float((quad.packed0 >> 16u) & 0xFFFFu) / 256.0;
    
    // Unpack faceDir from packed1 (low 3 bits - supports 0-7 range for 6 face directions)
    uint faceDir = quad.packed1 & 0x7u;
    
    // Get unit quad vertex ([0, 0] to [1, 1] - industry standard corner-based)
    vec3 unitPos = UNIT_QUAD_POSITIONS[vertexIndex];
    
    // Scale by quad dimensions (no center offset - direct corner positioning)
    vec3 scaledPos = unitPos * vec3(width, height, 1.0);
    
    // Rotate based on face direction to align with world axes
    vec3 rotatedPos = FACE_ROTATIONS[faceDir] * scaledPos;
    
    // Unpack islandID from packed2 (high 16 bits)
    uint islandID = (quad.packed2 >> 16u) & 0xFFFFu;
    
    // Reconstruct island transform matrix from vec4 array
    uint baseIdx = islandID * 4u;
    mat4 islandTransform = mat4(
        transformData[baseIdx + 0u],
        transformData[baseIdx + 1u],
        transformData[baseIdx + 2u],
        transformData[baseIdx + 3u]
    );
    
    // Combine: quad local geometry + island-relative position, then transform to world space
    vec3 islandRelativePos = rotatedPos + quad.position;
    vec3 worldPos = (islandTransform * vec4(islandRelativePos, 1.0)).xyz;
    
    // Apply view-projection
    gl_Position = pc.viewProjection * vec4(worldPos, 1.0);
    
    // Unpack normal from packed1 (bits 3-32: 10:10:10, with faceDir in bits 0-2)
    uint packedNormal = (quad.packed1 >> 3u);
    int nx = int((packedNormal >>  0u) & 0x3FFu) - 512;
    int ny = int((packedNormal >> 10u) & 0x3FFu) - 512;
    int nz = int((packedNormal >> 20u) & 0x3FFu) - 512;
    vec3 localNormal = normalize(vec3(float(nx), float(ny), float(nz)));
    
    // Extract rotation from island transform (upper 3x3) and apply to normal
    mat3 rotationMatrix = mat3(islandTransform);
    vNormal = normalize(rotationMatrix * localNormal);
    
    // Unpack blockType from packed2 (low 16 bits)
    uint blockType = quad.packed2 & 0xFFFFu;
    
    // Pass attributes with tiled texture coordinates
    // Multiply texcoords by quad dimensions to repeat texture instead of stretching
    vTexCoord = UNIT_QUAD_TEXCOORDS[vertexIndex] * vec2(width, height);
    vBlockType = blockType;
    vWorldPos = worldPos;
}
