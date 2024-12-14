#version 460 core

layout (location = 0) in vec3 position;
layout (location = 1) in vec2 texture_coords;

uniform mat4 projView_matrix;
uniform mat4 model_matrix;

void main() {
    gl_Position = projView_matrix * model_matrix * vec4(position, 1.0);
}