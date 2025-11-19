#version 450

// Vertex pulling: fetch QuadFace from SSBO using gl_VertexIndex
// Island-relative positions + per-island transforms

// QuadFace struct (48 bytes with std430 layout - array elements align to 16 bytes)
// Note: std430 aligns vec3 to 16 bytes, AND each array element to base alignment (16)
struct QuadFace {
    vec3 position;       // Island-relative CORNER position (12 bytes, offset 0, aligned to 16)
    float _padding0;     // 4 bytes padding (offset 12) - CRITICAL for std430 alignment
    float width;         // Quad width (4 bytes, offset 16)
    float height;        // Quad height (4 bytes, offset 20)
    uint packedNormal;   // 10:10:10:2 packed normal (4 bytes, offset 24)
    uint blockType;      // Block type ID (4 bytes, offset 28)
    uint faceDir;        // Face direction 0-5 (4 bytes, offset 32)
    uint islandID;       // Island ID for SSBO lookup (4 bytes, offset 36)
    // Implicit 8-byte padding here (offset 40-47) for array stride alignment to 16 bytes
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
    
    // Get unit quad vertex ([0, 0] to [1, 1] - industry standard corner-based)
    vec3 unitPos = UNIT_QUAD_POSITIONS[vertexIndex];
    
    // Scale by quad dimensions (no center offset - direct corner positioning)
    vec3 scaledPos = unitPos * vec3(quad.width, quad.height, 1.0);
    
    // Rotate based on face direction to align with world axes
    vec3 rotatedPos = FACE_ROTATIONS[quad.faceDir] * scaledPos;
    
    // Reconstruct island transform matrix from vec4 array
    uint baseIdx = quad.islandID * 4u;
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
    
    // Unpack normal (no transform applied - islands don't rotate yet)
    int nx = int((quad.packedNormal >>  0) & 0x3FF) - 512;
    int ny = int((quad.packedNormal >> 10) & 0x3FF) - 512;
    int nz = int((quad.packedNormal >> 20) & 0x3FF) - 512;
    vec3 normal = normalize(vec3(float(nx), float(ny), float(nz)));
    vNormal = normal;
    
    // Pass attributes with tiled texture coordinates
    // Multiply texcoords by quad dimensions to repeat texture instead of stretching
    vTexCoord = UNIT_QUAD_TEXCOORDS[vertexIndex] * vec2(quad.width, quad.height);
    vBlockType = quad.blockType;
    vWorldPos = worldPos;
}
