#version 460 core

layout (location = 0) in uint vertex;
layout (location = 1) in vec2 tex_coords;
layout (location = 2) in float light;

out vec2 tex_coords_frag;
out float light_frag;

uniform mat4 projView_matrix;
uniform vec3 sect_pos;

void main() {
	// unpacking data from the vertex
	vec3 position;
	position.x = float(vertex & 0x1Fu);
	position.y = float((vertex & 0x3E0u) >> 5u);
	position.z = float((vertex & 0x7C00u) >> 10u);

	position += sect_pos;

    gl_Position = projView_matrix * vec4(position, 1.0);
	tex_coords_frag = tex_coords;
	light_frag = light;
}