#version 460

struct FaceInstance {
    ivec4 origin_direction;
    uvec4 extents;
};

layout(set = 0, binding = 0, std430) readonly buffer FaceInstances {
    FaceInstance instances[];
} face_instances;

layout(push_constant) uniform PushConstants {
    mat4 projection_view;
    ivec4 world_origin;
    vec4 camera_fog;
} pc;

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out uint out_face_direction;
layout(location = 2) out float out_fog;

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
    const FaceInstance instance = face_instances.instances[gl_InstanceIndex];
    const uint corner = kTriangleCorners[gl_VertexIndex];
    const uint direction = uint(instance.origin_direction.w);
    vec3 local = faceCorner(direction, corner);
    if (direction <= 1u) {
        local.y *= float(instance.extents.x);
        local.z *= float(instance.extents.y);
    } else if (direction <= 3u) {
        local.x *= float(instance.extents.x);
        local.z *= float(instance.extents.y);
    } else {
        local.x *= float(instance.extents.x);
        local.y *= float(instance.extents.y);
    }
    ivec3 relative_origin = instance.origin_direction.xyz - pc.world_origin.xyz;
    if (relative_origin.x > 32768) relative_origin.x -= 65536;
    if (relative_origin.x < -32768) relative_origin.x += 65536;
    if (relative_origin.y > 32768) relative_origin.y -= 65536;
    if (relative_origin.y < -32768) relative_origin.y += 65536;
    const vec3 relative_position = vec3(relative_origin) + local;
    gl_Position = pc.projection_view * vec4(relative_position, 1.0);
    out_uv = direction <= 1u ? local.yz : direction <= 3u ? local.xz : local.xy;
    out_face_direction = direction;
    const vec2 horizontal_offset = abs(relative_position.xy - pc.camera_fog.xy);
    const float horizontal_distance = max(horizontal_offset.x, horizontal_offset.y);
    out_fog = smoothstep(pc.camera_fog.w * 0.55, pc.camera_fog.w, horizontal_distance);
}
