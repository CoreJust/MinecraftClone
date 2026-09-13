#version 460

// Vertex-shader fallback of grid.mesh for platforms without VK_EXT_mesh_shader.
// Each instance corresponds to one grid cell (one mesh workgroup).

layout(push_constant) uniform PushConstants {
    mat4 projection_view;
    float world_size;
    float line_width;
    float pad0;
    float pad1;
    vec4  line_color;
    vec4  surface_color;
} pc;

layout(location = 0) out vec4 outColor;

const uint kGridSize = 32u;

const uint kIndices[6] = uint[](0u, 1u, 2u, 2u, 1u, 3u);

vec2 rectangleCorner(uint rectangle, uint corner) {
    float w = pc.line_width * 0.5;
    vec4 bounds[5] = vec4[](
        vec4(w, w, 1.0 - w, 1.0 - w),
        vec4(0.0, 0.0, 1.0, w),
        vec4(0.0, 1.0 - w, 1.0, 1.0),
        vec4(0.0, w, w, 1.0 - w),
        vec4(1.0 - w, w, 1.0, 1.0 - w)
    );
    vec4 b = bounds[rectangle];
    return vec2((corner & 1u) == 0u ? b.x : b.z, corner < 2u ? b.y : b.w);
}

void main() {
    uint cell = uint(gl_InstanceIndex);
    vec2 xy = vec2(float(cell % kGridSize), float(cell / kGridSize));

    uint rectangle = uint(gl_VertexIndex) / 6u;
    uint corner = kIndices[uint(gl_VertexIndex) % 6u];
    vec2 p = xy + rectangleCorner(rectangle, corner);

    gl_Position = pc.projection_view * vec4(p, 0.0, 1.0);
    float line_visibility = 1.0 - smoothstep(18.0, 55.0, gl_Position.w);
    outColor = rectangle == 0u
        ? pc.surface_color
        : mix(pc.surface_color, pc.line_color, line_visibility);
}
