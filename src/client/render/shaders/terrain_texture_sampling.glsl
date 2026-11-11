layout(set = 0, binding = 1) uniform sampler2DArray terrain_texture_atlas;

layout(push_constant) uniform StoneOutputPushConstants {
    mat4 projection_view;
    ivec4 world_origin;
    vec4 camera_fog;
    vec4 curvature;
    vec4 surface;
} terrain_output;

const uint kTerrainFaceCount = 6u;
const uint kTerrainMaterialCount = 8u;
const uint kGrassMaterial = 2u;
const uint kSnowMaterial = 5u;
const uint kGrassSideOverlayTile = 48u;
const vec3 kGrassTintSrgb = vec3(145.0, 189.0, 89.0) / 255.0;

// Terrain texture samples are decoded from the sRGB image format by Vulkan.
vec3 srgbToLinear(vec3 encoded) {
    const bvec3 low_range = lessThanEqual(encoded, vec3(0.04045));
    const vec3 linear_low = encoded / 12.92;
    const vec3 linear_high = pow((encoded + 0.055) / 1.055, vec3(2.4));
    return mix(linear_high, linear_low, low_range);
}

vec3 linearToSrgb(vec3 linear) {
    const bvec3 low_range = lessThanEqual(linear, vec3(0.0031308));
    const vec3 linear_low = linear * 12.92;
    const vec3 linear_high = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
    return mix(linear_high, linear_low, low_range);
}

vec3 encodeTerrainOutput(vec3 linear_color) {
    // Presentation requests this on UNORM surfaces; linear offscreen captures do not.
    return terrain_output.camera_fog.z >= 0.5 ? linearToSrgb(linear_color) : linear_color;
}

vec4 sampleTerrainTexture(uint tile_index, vec2 uv) {
    // Keep UVs unwrapped so the sampler can derive the correct minification footprint.
    return texture(terrain_texture_atlas, vec3(uv, float(tile_index)));
}

vec3 sampleTerrainMaterial(uint material_id, uint face_direction, vec2 uv) {
    if (material_id == 0u || material_id >= kTerrainMaterialCount || face_direction >= kTerrainFaceCount) {
        return vec3(1.0, 0.0, 1.0);
    }
    const uint tile_index = material_id * kTerrainFaceCount + face_direction;
    const vec4 base_texel = sampleTerrainTexture(tile_index, uv);
    vec3 color = base_texel.rgb;
    if (material_id == kGrassMaterial) {
        const vec3 grass_tint = srgbToLinear(kGrassTintSrgb);
        if (face_direction < 4u) {
            const vec4 overlay_texel = sampleTerrainTexture(kGrassSideOverlayTile, uv);
            const vec3 overlay_color = overlay_texel.rgb * grass_tint;
            color = color * (1.0 - overlay_texel.a) + overlay_color;
        } else if (face_direction == 5u) {
            color *= grass_tint;
        }
    }
    return color;
}

vec3 shadeDistantVoxelRiser(vec3 lit_color, uint material, uint face_direction,
    vec2 camera_offset, float vertical_footprint_m) {
    if (face_direction >= 4u || (material != kGrassMaterial && material != kSnowMaterial)) {
        return lit_color;
    }
    // Keep the directional contrast of nearby faces and resolved risers.
    const float distance_m = length(camera_offset);
    const float grass_blend = max(smoothstep(0.30, 0.85, vertical_footprint_m),
        smoothstep(80.0, 128.0, distance_m));
    const float distant_blend = smoothstep(64.0, 112.0, distance_m)
        * (material == kSnowMaterial ? smoothstep(0.15, 0.45, vertical_footprint_m) : grass_blend);
    // At medium distance, retain some side shading while softening the Grass overlay's brown stripes.
    const float blend = material == kGrassMaterial
        ? max(distant_blend, 0.90 * smoothstep(48.0, 72.0, distance_m))
        : distant_blend;
    if (blend <= 0.0) {
        return lit_color;
    }
    const float top_tile = float(material * kTerrainFaceCount + 5u);
    const float mean_lod = float(textureQueryLevels(terrain_texture_atlas) - 1);
    vec3 top_mean = textureLod(terrain_texture_atlas, vec3(0.5, 0.5, top_tile), mean_lod).rgb;
    if (material == kGrassMaterial) {
        top_mean *= srgbToLinear(kGrassTintSrgb);
    }
    return mix(lit_color, top_mean, blend);
}

const float kTerrainFaceShades[6] = float[](0.68, 0.84, 0.76, 0.9, 0.62, 1.0);

uint interpolatedSurfaceMaterial(vec3 classification) {
    const bool cold = classification.z <= 0.0;
    if (classification.y < 0.0) return 1u;
    if (classification.x < 0.0) return cold ? 6u : 3u;
    return cold ? 5u : 2u;
}

layout(set = 0, binding = 2, std430) readonly buffer CanonicalTerrainField {
    uint resolution;
    uint entry_count;
    float sea_level;
    uint enabled;
    vec4 values[];
} canonical_terrain;

vec2 canonicalTerrainCoordinate(vec2 world_xy) {
    return mod(world_xy, vec2(65536.0)) * float(canonical_terrain.resolution) / 65536.0;
}

vec4 canonicalTerrainSample(vec2 world_xy) {
    const vec2 coordinate = canonicalTerrainCoordinate(world_xy);
    const uvec2 low = uvec2(floor(coordinate)) % canonical_terrain.resolution;
    const uvec2 high = (low + 1u) % canonical_terrain.resolution;
    const vec2 fraction = fract(coordinate);
    const vec4 a = canonical_terrain.values[low.y * canonical_terrain.resolution + low.x];
    const vec4 b = canonical_terrain.values[low.y * canonical_terrain.resolution + high.x];
    const vec4 c = canonical_terrain.values[high.y * canonical_terrain.resolution + low.x];
    const vec4 d = canonical_terrain.values[high.y * canonical_terrain.resolution + high.x];
    return mix(mix(a, b, fraction.x), mix(c, d, fraction.x), fraction.y);
}

uint v9CanonicalSurfaceMaterial(vec2 world_xy, vec3 classification) {
    if (classification.y < 0.0) return 1u;
    if (classification.x < 0.0) {
        const float temperature = canonicalTerrainSample(world_xy).x;
        return temperature <= -2.0 ? 6u : 3u;
    }
    const vec2 coordinate = canonicalTerrainCoordinate(world_xy);
    const uvec2 low = uvec2(floor(coordinate)) % canonical_terrain.resolution;
    const uvec2 high = (low + 1u) % canonical_terrain.resolution;
    const vec2 fraction = fract(coordinate);
    const uvec4 materials = uvec4(round(vec4(
        canonical_terrain.values[low.y * canonical_terrain.resolution + low.x].z,
        canonical_terrain.values[low.y * canonical_terrain.resolution + high.x].z,
        canonical_terrain.values[high.y * canonical_terrain.resolution + low.x].z,
        canonical_terrain.values[high.y * canonical_terrain.resolution + high.x].z)));
    const vec4 weights = vec4((1.0 - fraction.x) * (1.0 - fraction.y),
        fraction.x * (1.0 - fraction.y), (1.0 - fraction.x) * fraction.y,
        fraction.x * fraction.y);
    // Interpolate coverage, never numeric material IDs or their albedo colors.
    uint selected = materials.x;
    float selected_coverage = -1.0;
    for (int candidate = 0; candidate < 4; ++candidate) {
        const float coverage = dot(weights, vec4(equal(materials, uvec4(materials[candidate]))));
        if (coverage > selected_coverage) {
            selected = materials[candidate];
            selected_coverage = coverage;
        }
    }
    return selected;
}

uint triangleSurfaceMaterial(uint packed_materials, vec3 weights) {
    const uvec3 materials = uvec3(packed_materials & 255u,
        (packed_materials >> 8u) & 255u, (packed_materials >> 16u) & 255u);
    uint selected = materials.x;
    float selected_coverage = -1.0;
    for (int candidate = 0; candidate < 3; ++candidate) {
        const float coverage = dot(weights, vec3(equal(materials, uvec3(materials[candidate]))));
        if (coverage > selected_coverage
            || (coverage == selected_coverage && materials[candidate] < selected)) {
            selected = materials[candidate];
            selected_coverage = coverage;
        }
    }
    return selected;
}

uint canonicalSurfaceMaterial(vec2 world_xy, vec3 classification) {
    if (canonical_terrain.enabled == 0u) {
        return interpolatedSurfaceMaterial(classification);
    }
    if (canonical_terrain.enabled == 2u) {
        return v9CanonicalSurfaceMaterial(world_xy, classification);
    }
    const vec4 sample_value = canonicalTerrainSample(world_xy);
    const bool cold = sample_value.x <= -2.0;
    if (sample_value.y >= canonical_terrain.sea_level) {
        return cold ? 5u : 2u;
    }
    if (classification.y < 0.0) return 1u;
    return cold ? 6u : 3u;
}
