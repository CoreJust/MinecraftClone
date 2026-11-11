#version 460
#extension GL_GOOGLE_include_directive : require

#include "terrain_texture_sampling.glsl"
#include "world_curvature.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in uint in_face_direction;
layout(location = 2) in float in_fog;
layout(location = 3) flat in uint in_material_id;
layout(location = 4) in vec3 in_camera_offset;
layout(location = 5) flat in float in_surface_shade;
layout(location = 6) in vec3 in_surface_properties;
layout(location = 7) in vec2 in_canonical_world_xy;
layout(location = 8) in vec3 in_surface_classification;
layout(location = 9) in vec3 in_mesh_normal;
layout(location = 10) flat in uint in_grass_bank;
layout(location = 0) out vec4 out_color;

void main() {
    const float vertical_footprint_m = fwidth(in_uv.y);
    float fog = in_fog;
    if (in_camera_offset.z > 0.5) {
        if (length(in_camera_offset.xy) >= in_camera_offset.z) { discard; }
        fog = worldVisibilityFog(in_camera_offset.xy, in_camera_offset.z);
    }
    const uint material = in_surface_properties.z > 1.5
        ? triangleSurfaceMaterial(in_material_id, in_surface_classification)
        : (in_surface_properties.z > 0.5
            ? canonicalSurfaceMaterial(in_canonical_world_xy, in_surface_classification) : in_material_id);
    const vec3 nearest_color = sampleTerrainMaterial(material, in_face_direction, in_uv);
    float shade = in_surface_shade;
    if (dot(in_mesh_normal, in_mesh_normal) > 0.0) {
        shade = 0.65 + 0.35 * max(0.0, dot(normalize(in_mesh_normal), normalize(vec3(0.3, 0.4, 1.0))));
    }
    const vec3 lit_color = shadeDistantVoxelRiser(
        nearest_color * kTerrainFaceShades[in_face_direction] * shade,
        in_grass_bank != 0u && material == 3u ? kGrassMaterial : material,
        in_face_direction, in_camera_offset.xy, vertical_footprint_m);
    const vec3 linear_color = mix(lit_color, vec3(0.62, 0.75, 0.90), fog);
    out_color = vec4(encodeTerrainOutput(linear_color), 1.0);
}
