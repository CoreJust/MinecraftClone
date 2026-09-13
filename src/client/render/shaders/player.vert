#version 460

// Vertex-shader fallback of player.mesh for platforms without VK_EXT_mesh_shader.

layout(push_constant) uniform PushConstants {
    mat4 projection_view;
    vec4 origin;
    vec4 extent;
    vec4 color;
} pc;

layout(location = 0) out vec4 outColor;

const vec3 kCubeVertices[36] = vec3[](
    vec3(0,0,0), vec3(1,1,0), vec3(1,0,0), vec3(0,0,0), vec3(0,1,0), vec3(1,1,0),
    vec3(0,0,1), vec3(1,0,1), vec3(1,1,1), vec3(0,0,1), vec3(1,1,1), vec3(0,1,1),
    vec3(0,0,0), vec3(1,0,0), vec3(1,0,1), vec3(0,0,0), vec3(1,0,1), vec3(0,0,1),
    vec3(0,1,0), vec3(0,1,1), vec3(1,1,1), vec3(0,1,0), vec3(1,1,1), vec3(1,1,0),
    vec3(0,0,0), vec3(0,0,1), vec3(0,1,1), vec3(0,0,0), vec3(0,1,1), vec3(0,1,0),
    vec3(1,0,0), vec3(1,1,0), vec3(1,1,1), vec3(1,0,0), vec3(1,1,1), vec3(1,0,1)
);

const float kFaceShades[6] = float[](0.62, 1.0, 0.76, 0.9, 0.68, 0.84);

void main() {
    vec3 local = kCubeVertices[gl_VertexIndex];
    if (pc.extent.w > 0.5 && gl_VertexIndex >= 6 && gl_VertexIndex < 12) {
        local = kCubeVertices[6];
    }
    vec3 world = pc.origin.xyz + pc.extent.xyz * local;
    gl_Position = pc.projection_view * vec4(world, 1.0);
    outColor = vec4(pc.color.rgb * kFaceShades[gl_VertexIndex / 6], pc.color.a);
}
