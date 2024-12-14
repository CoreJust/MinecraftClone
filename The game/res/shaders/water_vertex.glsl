#version 460 core

layout (location = 0) in uint vertex;
layout (location = 1) in vec2 tex_coords;
layout (location = 2) in float light;

out vec2 tex_coords_frag;
out float light_frag;

uniform mat4 projView_matrix;
uniform vec3 sect_pos;
uniform float globalTime;

vec4 getWorldPos() {
	// unpacking data from the vertex
	vec3 position;
	position.x = float(vertex & 31u);
	position.y = float((vertex >> 5u) & 31u);
	position.z = float((vertex >> 10u) & 31u);

	position += sect_pos;

    vec3 inVert = position.xyz;
    inVert.y += sin((globalTime + inVert.x) * 1.5) / 9.8;
    inVert.y += cos((globalTime + inVert.z) * 1.5) / 9.1;
    inVert.y -= 0.25;
    return vec4(inVert, 1.0);
}

void main() {

    gl_Position = projView_matrix * getWorldPos();

    tex_coords_frag = tex_coords;
    light_frag = light;
}