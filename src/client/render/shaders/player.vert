#version 460
#extension GL_GOOGLE_include_directive : require
#include "world_curvature.glsl"

// Vertex-shader fallback of player.mesh for platforms without VK_EXT_mesh_shader.

layout(push_constant) uniform PushConstants {
    mat4 projection_view;
    vec4 origin;
    vec4 extent;
    vec4 color;
    vec4 curvature;
} pc;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec3 outCameraOffset;

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
    outCameraOffset = vec3(world.xy - pc.curvature.xy, pc.curvature.z * pc.curvature.w);
    if (pc.curvature.z > 0.5) { world = curvedWorldPosition(world, pc.curvature.xy); }
    gl_Position = pc.projection_view * vec4(world, 1.0);
    outColor = vec4(pc.color.rgb * kFaceShades[gl_VertexIndex / 6], pc.color.a);
}
