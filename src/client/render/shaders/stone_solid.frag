#version 460

layout(location = 1) flat in uint in_face_direction;
layout(location = 2) in float in_fog;
layout(location = 0) out vec4 out_color;

const float kFaceShades[6] = float[](0.68, 0.84, 0.76, 0.9, 0.62, 1.0);

void main() {
    const vec3 color = vec3(0.36499694, 0.36891850, 0.38051471);
    const vec3 lit_color = color * kFaceShades[in_face_direction];
    out_color = vec4(mix(lit_color, vec3(0.62, 0.75, 0.90), in_fog), 1.0);
}
