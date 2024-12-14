#version 460 core

out vec4 color;

in vec2 tex_coords_frag;
in float light_frag;

uniform sampler2D text;

void main() {
	float light = (light_frag + 1) / 17;
	color = texture(text, tex_coords_frag) * vec4(light, light, light, 1.0f);
	if (color.a == 0)
		discard;
}