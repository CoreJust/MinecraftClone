#version 460 core

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texture_coords;

out vec2 tex_coords_frag;

void main() {
	tex_coords_frag = texture_coords;
	gl_Position = vec4(position, 0.0, 1.0);
}