#version 460 core

layout (location = 0) in vec3 position;
layout (location = 1) in vec2 tex_coords;
layout (location = 2) in float light;

out vec2 tex_coords_frag;
out float light_frag;

uniform mat4 matrix;
uniform vec2 pos;

void main() {
    gl_Position = matrix * vec4(position, 1.0);
	gl_Position.xy += pos.xy;

	tex_coords_frag = tex_coords;
	light_frag = light;
}