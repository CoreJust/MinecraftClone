#version 460

layout(location = 0) out float out_vertical;

void main() {
    const vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2(3.0, -1.0),
        vec2(-1.0, 3.0)
    );
    const vec2 position = positions[gl_VertexIndex];
    gl_Position = vec4(position, 0.999999, 1.0);
    out_vertical = clamp((position.y + 1.0) * 0.5, 0.0, 1.0);
}
