#version 460 core

in vec2 tex_coords_frag;

out vec4 outColor;

uniform sampler2D screen;

void main() {
	outColor = texture(screen, tex_coords_frag);
	outColor.r *= 0.4f;
	outColor.g *= 0.5f;
}