#version 450 core

struct GuiVectorVertex {
    float x;
    float y;
    uint rgba;
};
layout(std430, set = 0, binding = 0) readonly buffer GuiVectorBuffer {
    GuiVectorVertex vertices[];
} vectors;
layout(push_constant) uniform GuiVectorParams {
    vec2 resolution;
} params;
layout(location = 0) out vec4 out_color;

vec3 srgbToLinear(vec3 color) {
    return mix(pow((color + 0.055) / 1.055, vec3(2.4)), color / 12.92, lessThanEqual(color, vec3(0.04045)));
}
void main() {
    GuiVectorVertex vertex = vectors.vertices[gl_VertexIndex];
    gl_Position = vec4(vec2(vertex.x, vertex.y) / params.resolution * 2.0 - 1.0, 0.0, 1.0);
    vec4 color = unpackUnorm4x8(vertex.rgba);
    out_color = vec4(srgbToLinear(color.rgb), color.a);
}
