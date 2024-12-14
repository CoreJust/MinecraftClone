#version 460 core

layout (location = 0) in vec3 position;
layout (location = 1) in vec2 tex_coords;
layout (location = 2) in float light;

out vec2 tex_coords_frag;
out float light_frag;

uniform mat4 projView_matrix;
uniform mat4 model_matrix;

void main() {
    gl_Position = projView_matrix * model_matrix * vec4(position, 1.0);
	tex_coords_frag = tex_coords;
	light_frag = light;
}