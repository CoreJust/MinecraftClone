#version 450

layout(push_constant) uniform ps {
    mat4 mvp;
};

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 outColor;

void main() {
    gl_Position = mvp * vec4(inPosition, 0.0, 1.0);
    outColor = inColor;
}
