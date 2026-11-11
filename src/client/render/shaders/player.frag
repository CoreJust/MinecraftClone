#version 460
#extension GL_GOOGLE_include_directive : require
#include "world_curvature.glsl"

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec3 inCameraOffset;
layout(location = 2) in vec3 inLocalPosition;
layout(location = 3) flat in float inBlockSeed;
layout(location = 0) out vec4 outColor;

uint crackHash(uint value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    return value ^ (value >> 16U);
}

float crackRandom(uint seed, uint channel) {
    uint value = crackHash(seed ^ channel * 0x9e3779b9U);
    return float(value & 0x00ffffffU) / 16777215.0;
}

vec2 crackBoundaryPoint(vec2 origin, vec2 direction) {
    direction = normalize(direction);
    vec2 distance_to_edge = vec2(1000.0);
    if (direction.x > 0.0001) { distance_to_edge.x = (15.0 - origin.x) / direction.x; }
    if (direction.x < -0.0001) { distance_to_edge.x = -origin.x / direction.x; }
    if (direction.y > 0.0001) { distance_to_edge.y = (15.0 - origin.y) / direction.y; }
    if (direction.y < -0.0001) { distance_to_edge.y = -origin.y / direction.y; }
    return origin + direction * min(distance_to_edge.x, distance_to_edge.y);
}

float revealedCrackSegmentCoverage(vec2 pixel, vec2 start, vec2 end, float progress,
    float reveal_start, float reveal_end, float half_width)
{
    float reveal = clamp((progress - reveal_start) / (reveal_end - reveal_start), 0.0, 1.0);
    if (reveal <= 0.0) { return 0.0; }
    vec2 revealed_end = mix(start, end, reveal);
    vec2 segment = revealed_end - start;
    float segment_length_squared = max(dot(segment, segment), 0.0001);
    float along = clamp(dot(pixel - start, segment) / segment_length_squared, 0.0, 1.0);
    float distance = length(pixel - (start + segment * along));
    float antialias_width = clamp(fwidth(distance), 0.015, half_width * 0.5);
    return 1.0 - smoothstep(half_width - antialias_width, half_width + antialias_width, distance);
}

float proceduralCrackCoverage() {
    float progress = clamp(-inColor.a, 0.0, 1.0);
    // Each cube face has one local coordinate fixed at zero or one. Avoid derivatives across face seams.
    bool x_face = inLocalPosition.x < 0.0001 || inLocalPosition.x > 0.9999;
    bool y_face = inLocalPosition.y < 0.0001 || inLocalPosition.y > 0.9999;
    int face_axis = x_face ? 0 : y_face ? 1 : 2;
    vec2 uv = face_axis == 0 ? inLocalPosition.yz
        : face_axis == 1 ? inLocalPosition.xz : inLocalPosition.xy;
    float side = step(0.5, inLocalPosition[face_axis]);
    uint block_seed = uint(round(inBlockSeed * 16777215.0));
    uint face_id = uint(face_axis) * 2U + uint(side);
    uint face_seed = crackHash(block_seed ^ face_id * 0x9e3779b9U);

    // Keep the crack anchored to the same 16x16 face grid while allowing stable subpixel coverage.
    vec2 pixel = clamp(uv, vec2(0.0), vec2(0.9999)) * 16.0;
    vec2 impact = vec2(5.0 + crackRandom(face_seed, 0U) * 6.0,
        5.0 + crackRandom(face_seed, 1U) * 6.0);
    float rotation = crackRandom(face_seed, 2U) * 6.2831853;
    float coverage = 0.0;
    // Each ray grows through two irregular joints. Later splits begin only
    // after their supporting joint has been reached, so fractures stay connected.
    for (uint ray = 0U; ray < 6U; ++ray) {
        uint ray_seed = crackHash(face_seed ^ (ray + 1U) * 0x85ebca6bU);
        float angle = rotation + (float(ray) + (crackRandom(ray_seed, 0U) - 0.5) * 0.55)
            * (6.2831853 / 6.0);
        vec2 direction = vec2(cos(angle), sin(angle));
        vec2 perpendicular = vec2(-direction.y, direction.x);
        vec2 edge = crackBoundaryPoint(impact, direction);
        vec2 first = clamp(mix(impact, edge, 0.30 + 0.08 * crackRandom(ray_seed, 1U))
            + perpendicular * ((crackRandom(ray_seed, 2U) - 0.5) * 2.0), vec2(0.2), vec2(15.0));
        vec2 second = clamp(mix(impact, edge, 0.66 + 0.08 * crackRandom(ray_seed, 3U))
            + perpendicular * ((crackRandom(ray_seed, 4U) - 0.5) * 2.8), vec2(0.2), vec2(15.0));
        float start = 0.02 + float(ray) * 0.035;
        coverage = max(coverage, revealedCrackSegmentCoverage(pixel, impact, first,
            progress, start, start + 0.19, 0.16));
        coverage = max(coverage, revealedCrackSegmentCoverage(pixel, first, second,
            progress, start + 0.19, start + 0.39, 0.15));
        coverage = max(coverage, revealedCrackSegmentCoverage(pixel, second, edge,
            progress, start + 0.39, start + 0.65, 0.13));

        float branch_side = crackRandom(ray_seed, 5U) < 0.5 ? -1.0 : 1.0;
        vec2 branch_direction = normalize(direction * 0.55 + perpendicular * branch_side);
        float branch_length = 2.0 + 2.5 * crackRandom(ray_seed, 6U);
        vec2 branch_joint = clamp(first + branch_direction * branch_length * 0.5
            + direction * (crackRandom(ray_seed, 7U) - 0.5), vec2(0.2), vec2(15.0));
        vec2 branch_end = clamp(first + branch_direction * branch_length
            + direction * (crackRandom(ray_seed, 8U) - 0.5), vec2(0.2), vec2(15.0));
        coverage = max(coverage, revealedCrackSegmentCoverage(pixel, first, branch_joint,
            progress, start + 0.34, start + 0.52, 0.11));
        coverage = max(coverage, revealedCrackSegmentCoverage(pixel, branch_joint, branch_end,
            progress, start + 0.52, start + 0.78, 0.10));
    }
    return coverage;
}

void main() {
    if (inColor.a < 0.0) {
        float crack_coverage = proceduralCrackCoverage();
        if (crack_coverage <= 0.01) { discard; }
        outColor = vec4(inColor.rgb * mix(0.70, 1.0, crack_coverage), 1.0);
    } else {
        outColor = inColor;
    }
    if (inCameraOffset.z > 0.5) {
        if (length(inCameraOffset.xy) >= inCameraOffset.z) { discard; }
        outColor.rgb = mix(outColor.rgb, vec3(0.62, 0.75, 0.90), worldVisibilityFog(inCameraOffset.xy, inCameraOffset.z));
    }
}
