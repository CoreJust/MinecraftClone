#version 450

layout(push_constant) uniform ps {
    layout(row_major) mat4 mvp;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoords;

layout(location = 0) out vec2 outTexCoords;

void main() {
    gl_Position = mvp * vec4(inPosition, 1.0);
    outTexCoords = inTexCoords;
}
