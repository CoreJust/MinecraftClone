#version 460

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in uint in_face_direction;
layout(location = 2) in float in_fog;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 1) uniform samplerBuffer stone_texture;

const float kFaceShades[6] = float[](0.68, 0.84, 0.76, 0.9, 0.62, 1.0);

void main() {
    const uvec2 pixel = min(uvec2(fract(in_uv) * 16.0), uvec2(15u));
    const vec3 color = texelFetch(stone_texture, int(pixel.y * 16u + pixel.x)).rgb;
    const vec3 lit_color = color * kFaceShades[in_face_direction];
    out_color = vec4(mix(lit_color, vec3(0.62, 0.75, 0.90), in_fog), 1.0);
}
