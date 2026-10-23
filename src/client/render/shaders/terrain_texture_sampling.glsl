layout(set = 0, binding = 1) uniform samplerBuffer terrain_texture_atlas;

layout(push_constant) uniform StoneOutputPushConstants {
    mat4 projection_view;
    ivec4 world_origin;
    vec4 camera_fog;
    vec4 curvature;
    vec4 surface;
} terrain_output;

const uint kTerrainTileSize = 16u;
const uint kTerrainPixelsPerTile = 256u;
const uint kTerrainFaceCount = 6u;
const uint kTerrainMaterialCount = 7u;
const uint kGrassMaterial = 2u;
const uint kGrassSideOverlayTile = 42u;
const vec3 kGrassTintSrgb = vec3(145.0, 189.0, 89.0) / 255.0;

// The atlas view is UNORM, so decode PNG RGB before linear-light blending and shading.
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
    const uvec2 pixel = min(
        uvec2(fract(uv) * float(kTerrainTileSize)),
        uvec2(kTerrainTileSize - 1u)
    );
    const uint texel_index = tile_index * kTerrainPixelsPerTile
        + pixel.y * kTerrainTileSize + pixel.x;
    return texelFetch(terrain_texture_atlas, int(texel_index));
}

vec3 sampleTerrainMaterial(uint material_id, uint face_direction, vec2 uv) {
    if (material_id == 0u || material_id >= kTerrainMaterialCount || face_direction >= kTerrainFaceCount) {
        return vec3(1.0, 0.0, 1.0);
    }
    const uint tile_index = material_id * kTerrainFaceCount + face_direction;
    const vec4 base_texel = sampleTerrainTexture(tile_index, uv);
    vec3 color = srgbToLinear(base_texel.rgb);
    if (material_id == kGrassMaterial) {
        const vec3 grass_tint = srgbToLinear(kGrassTintSrgb);
        if (face_direction < 4u) {
            const vec4 overlay_texel = sampleTerrainTexture(kGrassSideOverlayTile, uv);
            const vec3 overlay_color = srgbToLinear(overlay_texel.rgb) * grass_tint;
            color = mix(color, overlay_color, overlay_texel.a);
        } else if (face_direction == 5u) {
            color *= grass_tint;
        }
    }
    return color;
}

const float kTerrainFaceShades[6] = float[](0.68, 0.84, 0.76, 0.9, 0.62, 1.0);

uint interpolatedSurfaceMaterial(vec2 height_temperature) {
    const float height = floor(height_temperature.x);
    const float sea = terrain_output.surface.x;
    const bool cold = height_temperature.y <= -2.0;
    if (height < sea - 4.0) return 1u;
    if (height < sea) return cold ? 6u : 3u;
    return cold ? 5u : 2u;
}

layout(set = 0, binding = 2, std430) readonly buffer CanonicalTerrainField {
    uint resolution;
    uint entry_count;
    float sea_level;
    uint enabled;
    vec2 values[];
} canonical_terrain;

vec2 canonicalTerrainSample(vec2 world_xy) {
    const vec2 coordinate = mod(world_xy, vec2(65536.0)) * float(canonical_terrain.resolution) / 65536.0;
    const uvec2 low = uvec2(floor(coordinate)) % canonical_terrain.resolution;
    const uvec2 high = (low + 1u) % canonical_terrain.resolution;
    const vec2 fraction = fract(coordinate);
    const vec2 a = canonical_terrain.values[low.y * canonical_terrain.resolution + low.x];
    const vec2 b = canonical_terrain.values[low.y * canonical_terrain.resolution + high.x];
    const vec2 c = canonical_terrain.values[high.y * canonical_terrain.resolution + low.x];
    const vec2 d = canonical_terrain.values[high.y * canonical_terrain.resolution + high.x];
    return mix(mix(a, b, fraction.x), mix(c, d, fraction.x), fraction.y);
}

uint canonicalSurfaceMaterial(vec2 world_xy, vec2 height_temperature) {
    if (canonical_terrain.enabled == 0u) {
        return interpolatedSurfaceMaterial(height_temperature);
    }
    const vec2 sample_value = canonicalTerrainSample(world_xy);
    const bool cold = sample_value.x <= -2.0;
    if (sample_value.y >= canonical_terrain.sea_level) {
        return cold ? 5u : 2u;
    }
    if (floor(height_temperature.x) < terrain_output.surface.x - 4.0) return 1u;
    return cold ? 6u : 3u;
}
