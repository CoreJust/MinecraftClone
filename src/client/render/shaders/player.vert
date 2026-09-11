#version 460

// Vertex-shader fallback of player.mesh for platforms without VK_EXT_mesh_shader.

layout(push_constant) uniform PushConstants {
    mat4 projection_view;
    vec2 origin;
    float size;
    float pad0;
    vec4 color;
} pc;

layout(location = 0) out vec4 outColor;

const vec3 kCubeVertices[36] = vec3[](
    vec3(0,0,0), vec3(1,0,0), vec3(1,1,0), vec3(0,0,0), vec3(1,1,0), vec3(0,1,0),
    vec3(0,0,1), vec3(1,1,1), vec3(1,0,1), vec3(0,0,1), vec3(0,1,1), vec3(1,1,1),
    vec3(0,0,0), vec3(1,0,1), vec3(1,0,0), vec3(0,0,0), vec3(0,0,1), vec3(1,0,1),
    vec3(0,1,0), vec3(1,1,1), vec3(0,1,1), vec3(0,1,0), vec3(1,1,0), vec3(1,1,1),
    vec3(0,0,0), vec3(0,1,1), vec3(0,0,1), vec3(0,0,0), vec3(0,1,0), vec3(0,1,1),
    vec3(1,0,0), vec3(1,1,1), vec3(1,1,0), vec3(1,0,0), vec3(1,0,1), vec3(1,1,1)
);

void main() {
    vec3 world = vec3(pc.origin, 0.0) + kCubeVertices[gl_VertexIndex] * pc.size;
    gl_Position = pc.projection_view * vec4(world, 1.0);
    outColor = pc.color;
}
