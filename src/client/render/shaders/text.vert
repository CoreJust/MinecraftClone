#version 450 core

struct TextGlyph {
    uint character;
    uint packed_color;
    float x;
    float y;
    float z;
    float padding;
    float right_x;
    float right_y;
    float right_z;
    float up_x;
    float up_y;
    float up_z;
    float width;
    float height;
};

layout(std430, set = 0, binding = 0) readonly buffer TextGlyphBuffer {
    TextGlyph glyphs[];
} text;

layout(push_constant) uniform TextParams {
    mat4 projection_view;
    vec2 resolution;
    float scale;
    uint mode;
} params;

layout(location = 0) out vec2 out_pixel_position;
layout(location = 1) flat out uint out_character;
layout(location = 2) flat out uint out_packed_color;

const uint TEXT_MODE_SCREEN = 0u;
const uint TEXT_MODE_WORLD = 1u;

const vec2 SCREEN_QUAD_VERTICES[6] = {
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(1.0, 0.0),
    vec2(0.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0),
};

const vec2 WORLD_QUAD_VERTICES[6] = {
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(1.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(0.0, 1.0),
};

void main() {
    TextGlyph glyph = text.glyphs[gl_InstanceIndex];
    const vec2 quad_vertex = params.mode == TEXT_MODE_SCREEN
        ? SCREEN_QUAD_VERTICES[gl_VertexIndex]
        : WORLD_QUAD_VERTICES[gl_VertexIndex];
    vec2 vertex_position = quad_vertex * vec2(glyph.width, glyph.height) * params.scale;
    vec3 position = vec3(glyph.x, glyph.y, glyph.z) * params.scale;
    if (params.mode == TEXT_MODE_SCREEN) {
        vec2 pixel = position.xy + vertex_position;
        gl_Position = vec4(
            (pixel.x / params.resolution.x) * 2.0 - 1.0,
            (pixel.y / params.resolution.y) * 2.0 - 1.0,
            0.0,
            1.0
        );
    } else if (params.mode == TEXT_MODE_WORLD) {
        vec3 world_position = position
            + vec3(glyph.right_x, glyph.right_y, glyph.right_z) * vertex_position.x
            + vec3(glyph.up_x, glyph.up_y, glyph.up_z) * vertex_position.y;
        gl_Position = params.projection_view * vec4(world_position, 1.0);
    } else {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
    }
    out_character = glyph.character;
    out_packed_color = glyph.packed_color;
    out_pixel_position = quad_vertex * vec2(8.0, 16.0);
}
