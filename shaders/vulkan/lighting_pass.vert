#version 450

// Fullscreen triangle (no vertex buffer needed)
// Vertex 0: (-1, -1) -> UV (0, 0)
// Vertex 1: ( 3, -1) -> UV (2, 0)
// Vertex 2: (-1,  3) -> UV (0, 2)

layout(location = 0) out vec2 vTexCoord;

void main() {
    // Generate fullscreen triangle from vertex ID
    vTexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vTexCoord * 2.0 - 1.0, 0.0, 1.0);
}
