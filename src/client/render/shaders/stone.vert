#version 460

layout(set = 0, binding = 0, std430) readonly buffer FaceInstances {
    uvec4 instances[];
} face_instances;

layout(push_constant) uniform PushConstants {
    mat4 projection_view;
} pc;

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out uint out_face_direction;

const uint kTriangleCorners[6] = uint[](0u, 1u, 2u, 0u, 2u, 3u);

vec3 faceCorner(uint direction, uint corner) {
    const vec3 corners[24] = vec3[](
        vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0),
        vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0), vec3(1.0, 1.0, 1.0), vec3(1.0, 0.0, 1.0),
        vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0),
        vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 1.0), vec3(1.0, 1.0, 1.0), vec3(1.0, 1.0, 0.0),
        vec3(0.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0),
        vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 1.0), vec3(1.0, 1.0, 1.0), vec3(0.0, 1.0, 1.0)
    );
    return corners[direction * 4u + corner];
}

void main() {
    const uvec4 instance = face_instances.instances[gl_InstanceIndex];
    const uint corner = kTriangleCorners[gl_VertexIndex];
    const vec3 local = faceCorner(instance.w, corner);
    gl_Position = pc.projection_view * vec4(vec3(instance.xyz) + local, 1.0);
    out_uv = vec2(
        (corner == 1u || corner == 2u) ? 1.0 : 0.0,
        (corner >= 2u) ? 1.0 : 0.0
    );
    out_face_direction = instance.w;
}
