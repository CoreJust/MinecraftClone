#version 450

// layout(set = 0, binding = 0) uniform sampler2D textureAtlas;

layout(location = 0) in vec2 inTexCoords;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(inTexCoords, 1.0, 1.0); //texture(textureAtlas, inTexCoords);
}
