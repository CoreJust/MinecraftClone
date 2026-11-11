#version 460

layout(set = 0, binding = 0) uniform sampler2D map_image;
layout(push_constant) uniform MapImagePushConstants {
    uvec2 viewport_size;
    uvec2 image_size;
    uint encode_srgb;
    uint cropped;
    uint decode_srgb;
    uint padding;
    vec4 source;
    vec4 destination;
} push_constants;

layout(location = 0) out vec4 out_color;

vec3 srgbToLinear(vec3 color) {
    return mix(pow((color + 0.055) / 1.055, vec3(2.4)), color / 12.92, lessThanEqual(color, vec3(0.04045)));
}

vec3 linearToSrgb(vec3 linear_color) {
    const vec3 low = linear_color * 12.92;
    const vec3 high = 1.055 * pow(max(linear_color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear_color, vec3(0.0031308)));
}

void main() {
    const vec2 viewport = vec2(push_constants.viewport_size);
    const vec2 image_size = vec2(push_constants.image_size);
    const float scale = min(viewport.x / image_size.x, viewport.y / image_size.y);
    const vec2 fitted_size = push_constants.cropped != 0u ? push_constants.destination.zw : image_size * scale;
    const vec2 origin = push_constants.cropped != 0u ? push_constants.destination.xy : (viewport - fitted_size) * 0.5;
    const vec2 image_position = gl_FragCoord.xy - origin;
    if (any(lessThan(image_position, vec2(0.0))) || any(greaterThanEqual(image_position, fitted_size))) {
        if (push_constants.cropped != 0u) { discard; }
        const vec3 letterbox = vec3(0.035, 0.043, 0.055);
        out_color = vec4(
            push_constants.encode_srgb != 0U ? linearToSrgb(letterbox) : letterbox,
            1.0
        );
        return;
    }
    const vec2 uv = image_position / fitted_size;
    const vec2 coordinate = (push_constants.cropped != 0u
        ? push_constants.source.xy + uv * push_constants.source.zw : uv) * image_size;
    const ivec2 pixel = ivec2(min(floor(coordinate), image_size - 1.0));
    vec4 color = texelFetch(map_image, pixel, 0);
    if (push_constants.decode_srgb != 0u) { color.rgb = srgbToLinear(color.rgb); }
    out_color = vec4(
        push_constants.encode_srgb != 0U ? linearToSrgb(color.rgb) : color.rgb,
        color.a
    );
}
