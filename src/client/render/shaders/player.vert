#version 460

// Vertex-shader fallback of player.mesh for platforms without VK_EXT_mesh_shader.

layout(push_constant) uniform PushConstants {
    vec2 origin;
    float size;
    float pad0;
    vec4 color;
} pc;

layout(location = 0) out vec4 outColor;

const vec2 kCorners[4] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);
const uint kIndices[6] = uint[](0u, 1u, 2u, 2u, 1u, 3u);

vec2 worldToNdc(vec2 p) {
    vec2 ndc;
    ndc.x = (p.x / 32.0) * 2.0 - 1.0;
    ndc.y = (p.y / 32.0) * 2.0 - 1.0;
    return ndc;
}

void main() {
    vec2 c = kCorners[kIndices[gl_VertexIndex]];
    gl_Position = vec4(worldToNdc(pc.origin + c * pc.size), 0.0, 1.0);
    outColor = pc.color;
}
