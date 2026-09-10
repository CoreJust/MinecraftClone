#version 460

// Vertex-shader fallback of grid.mesh for platforms without VK_EXT_mesh_shader.
// Each instance corresponds to one grid cell (one mesh workgroup).

layout(push_constant) uniform PushConstants {
    float world_size;
    float line_width;
    float pad0;
    float pad1;
    vec4  line_color;
} pc;

layout(location = 0) out vec4 outColor;

const uint kGridSize = 32u;

const vec2 kCorners[4] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);
const uint kIndices[6] = uint[](0u, 1u, 2u, 2u, 1u, 3u);

vec2 worldToNdc(vec2 p) {
    return vec2(
        (p.x / pc.world_size) * 2.0 - 1.0,
        (p.y / pc.world_size) * 2.0 - 1.0
    );
}

void main() {
    uint cell = uint(gl_InstanceIndex);
    vec2 xy = vec2(float(cell % kGridSize), float(cell / kGridSize));

    vec2 c = kCorners[kIndices[gl_VertexIndex]];
    vec2 inset = vec2(pc.line_width * 0.5);
    vec2 p = xy + inset + c * (1.0 - pc.line_width);

    gl_Position = vec4(worldToNdc(p), 0.0, 1.0);
    outColor = pc.line_color;
}
