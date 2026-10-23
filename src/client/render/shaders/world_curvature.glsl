const float WORLD_CURVATURE_RADIUS = 131072.0;
const float MIN_WORLD_VISIBILITY_RADIUS = 16384.0;
const float MAX_WORLD_VISIBILITY_RADIUS = 32768.0;
const float WORLD_VISIBILITY_SEA_LEVEL = 384.0;

float worldVisibilityRadius(float cameraWorldZ) {
    float height = max(cameraWorldZ - WORLD_VISIBILITY_SEA_LEVEL, 0.0);
    return clamp(sqrt(2.0 * WORLD_CURVATURE_RADIUS * height),
        MIN_WORLD_VISIBILITY_RADIUS, MAX_WORLD_VISIBILITY_RADIUS);
}

vec3 curvedWorldPosition(vec3 position, vec2 cameraXY) {
    vec2 offset = position.xy - cameraXY;
    position.z -= dot(offset, offset) / (2.0 * WORLD_CURVATURE_RADIUS);
    return position;
}

float worldVisibilityFog(vec2 cameraOffset, float visibilityRadius) {
    return smoothstep(0.96 * visibilityRadius, visibilityRadius, length(cameraOffset));
}
