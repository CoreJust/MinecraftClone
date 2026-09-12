#version 450 core

layout(push_constant) uniform DebugHudParams {
    vec2 resolution;
    float scale;
    uint packed_ascii[28];
} params;

layout(location = 0) out vec2 out_pixel_position;
layout(location = 1) flat out uint out_packed_ascii;

const float GLYPH_SCALE = 2.0;
const float GLYPH_ADVANCE = 64.0;
const float HUD_MARGIN = 8.0;
const float HUD_LINE_ADVANCE = 40.0;
const uint WORDS_PER_LINE = 7u;
const vec2 QUAD_VERTICES[6] = {
    vec2(0.0, 0.0),
    vec2(64.0, 32.0),
    vec2(64.0, 0.0),
    vec2(0.0, 0.0),
    vec2(0.0, 32.0),
    vec2(64.0, 32.0),
};

void main() {
    const vec2 vertex_position = QUAD_VERTICES[gl_VertexIndex];
    const uint line = gl_InstanceIndex / WORDS_PER_LINE;
    const uint column = gl_InstanceIndex % WORDS_PER_LINE;
    const float max_horizontal_scale = params.resolution.x
        / (HUD_MARGIN + float(WORDS_PER_LINE) * GLYPH_ADVANCE);
    const float max_vertical_scale = params.resolution.y
        / (HUD_MARGIN + 3.0 * HUD_LINE_ADVANCE + 32.0);
    const float layout_scale = min(params.scale, min(max_horizontal_scale, max_vertical_scale));
    const float x = (HUD_MARGIN + vertex_position.x + float(column) * GLYPH_ADVANCE) * layout_scale;
    const float y = (HUD_MARGIN + vertex_position.y + float(line) * HUD_LINE_ADVANCE) * layout_scale;
    const vec2 normalized = vec2(
        (x / params.resolution.x) * 2.0 - 1.0,
        (y / params.resolution.y) * 2.0 - 1.0
    );
    gl_Position = vec4(normalized, 0.0, 1.0);
    out_packed_ascii = params.packed_ascii[gl_InstanceIndex];
    out_pixel_position = vertex_position / GLYPH_SCALE;
}
