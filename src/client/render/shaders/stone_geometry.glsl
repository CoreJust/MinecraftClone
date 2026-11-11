#include "world_curvature.glsl"

struct FaceInstance {
    ivec4 origin_direction;
    uvec4 extents;
    uvec4 terrain_normals;
};

layout(set = 0, binding = 0, std430) readonly buffer FaceInstances {
    FaceInstance instances[];
} face_instances;

layout(push_constant) uniform PushConstants {
    mat4 projection_view;
    ivec4 world_origin;
    vec4 camera_fog;
    vec4 curvature;
    vec4 surface;
} pc;

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out uint out_face_direction;
layout(location = 2) out float out_fog;
layout(location = 3) flat out uint out_material_id;
layout(location = 4) out vec3 out_camera_offset;
layout(location = 5) flat out float out_surface_shade;
layout(location = 6) out vec3 out_surface_properties;
layout(location = 7) out vec2 out_canonical_world_xy;
layout(location = 8) out vec3 out_surface_classification;
layout(location = 9) out vec3 out_mesh_normal;
layout(location = 10) flat out uint out_grass_bank;

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
#ifdef MC_SPECIALIZED_INDEXED_BLOCK
    const uint face = uint(gl_VertexIndex) / 4u;
    const FaceInstance instance = face_instances.instances[face];
    const uint corner = uint(gl_VertexIndex) % 4u;
#else
    const bool indexed_block = pc.world_origin.w == 2;
    const uint face = indexed_block ? uint(gl_VertexIndex) / 4u : uint(gl_InstanceIndex);
    const FaceInstance instance = face_instances.instances[face];
    const uint corner = indexed_block ? uint(gl_VertexIndex) % 4u
        : pc.world_origin.w != 0 ? uint(gl_VertexIndex) : kTriangleCorners[gl_VertexIndex];
#endif
    const uint packed_direction = uint(instance.origin_direction.w);
    const uint direction = packed_direction & 7u;
    const bool xy_q5 = direction == 6u && (packed_direction & 64u) != 0u;
    vec3 local;
    out_surface_shade = 1.0;
    out_mesh_normal = vec3(0.0);
    out_surface_properties = vec3(0.0);
    out_surface_classification = vec3(0.0);
    if (direction == 6u) {
        const uint heights = uint(instance.origin_direction.z);
        const uint h0 = (heights & 65535u) | (((packed_direction >> 3u) & 1u) << 16u);
        const uint h1 = (heights >> 16u) | (((packed_direction >> 4u) & 1u) << 16u);
        const uint h2 = (instance.extents.w & 65535u) | (((packed_direction >> 5u) & 1u) << 16u);
        const vec3 v0 = vec3(0.0, 0.0, float(h0) / 32.0);
        const float xy_scale = xy_q5 ? 1.0 / 32.0 : 1.0;
        const vec3 v1 = vec3(float(int(instance.extents.x << 16u) >> 16) * xy_scale,
            float(int(instance.extents.x) >> 16) * xy_scale, float(h1) / 32.0);
        const vec3 v2 = vec3(float(int(instance.extents.y << 16u) >> 16) * xy_scale,
            float(int(instance.extents.y) >> 16) * xy_scale, float(h2) / 32.0);
        local = corner == 1u ? v1 : corner == 2u ? v2 : v0;
        if ((instance.extents.w & 0x80000000u) != 0u) {
            const float t0 = float(int(instance.extents.w << 1u) >> 17) / 32.0;
            const float t1 = float(int(instance.extents.z << 16u) >> 16) / 32.0;
            const float t2 = float(int(instance.extents.z) >> 16) / 32.0;
            out_surface_properties = vec3(local.z - 1.0, corner == 1u ? t1 : corner == 2u ? t2 : t0, 1.0);
            // Center each discrete cutoff before interpolation: a boundary stays exact zero.
            out_surface_classification = vec3(
                out_surface_properties.x - pc.surface.x,
                out_surface_properties.x - (pc.surface.x - 4.0),
                out_surface_properties.y + 2.0
            );
        } else if ((instance.extents.w & 0x40000000u) != 0u) {
            out_surface_properties.z = 2.0;
            out_surface_classification = corner == 1u ? vec3(0.0, 1.0, 0.0)
                : corner == 2u ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
        }
        const vec3 normal_cross = cross(v1 - v0, v2 - v0);
        const float normal_length_squared = dot(normal_cross, normal_cross);
        const vec3 normal = normal_length_squared > 0.0
            ? normal_cross * inversesqrt(normal_length_squared) : vec3(0.0, 0.0, 1.0);
        out_surface_shade = 0.65 + 0.35 * max(0.0, dot(normal, normalize(vec3(0.3, 0.4, 1.0))));
        if (instance.terrain_normals.w != 0u) {
            const uint packed = corner == 1u ? instance.terrain_normals.y
                : corner == 2u ? instance.terrain_normals.z : instance.terrain_normals.x;
            const vec2 oct = unpackSnorm2x16(packed);
            out_mesh_normal = normalize(vec3(oct, max(0.0, 1.0 - abs(oct.x) - abs(oct.y))));
        }
    } else {
        local = faceCorner(direction, corner);
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
    }
    ivec2 origin_xy = xy_q5 ? instance.origin_direction.xy / 32 : instance.origin_direction.xy;
    vec2 origin_fraction = xy_q5 ? vec2(instance.origin_direction.xy % 32) / 32.0 : vec2(0.0);
    out_canonical_world_xy = vec2(origin_xy) + origin_fraction + local.xy;
    ivec3 relative_origin = ivec3(origin_xy - pc.world_origin.xy,
        direction == 6u ? -pc.world_origin.z : instance.origin_direction.z - pc.world_origin.z);
    if (relative_origin.x > 32768) relative_origin.x -= 65536;
    if (relative_origin.x < -32768) relative_origin.x += 65536;
    if (relative_origin.y > 32768) relative_origin.y -= 65536;
    if (relative_origin.y < -32768) relative_origin.y += 65536;
    const vec3 relative_position = vec3(relative_origin) + vec3(origin_fraction, 0.0) + local;
    out_camera_offset = vec3(relative_position.xy - pc.curvature.xy, pc.curvature.z * pc.curvature.w);
    const vec3 rendered_position = pc.curvature.z > 0.5
        ? curvedWorldPosition(relative_position, pc.curvature.xy) : relative_position;
    gl_Position = pc.projection_view * vec4(rendered_position, 1.0);
    out_uv = direction <= 1u ? vec2(local.y, -local.z)
        : direction <= 3u ? vec2(local.x, -local.z)
        : origin_fraction + local.xy;
    out_face_direction = min(direction, 5u);
    out_material_id = instance.extents.z;
    out_grass_bank = direction < 4u ? instance.extents.w & 1u : 0u;
    const vec2 horizontal_offset = abs(relative_position.xy - pc.camera_fog.xy);
    const float horizontal_distance = max(horizontal_offset.x, horizontal_offset.y);
    out_fog = smoothstep(pc.camera_fog.w * 0.55, pc.camera_fog.w, horizontal_distance);
}
