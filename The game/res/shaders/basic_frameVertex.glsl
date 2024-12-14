#version 460 core

layout (location = 0) in vec3 position;
layout (location = 1) in vec2 tex_coords;

out vec2 tex_coords_frag;

void main() {
    gl_Position = vec4(position, 1.0);
	tex_coords_frag = tex_coords;
}